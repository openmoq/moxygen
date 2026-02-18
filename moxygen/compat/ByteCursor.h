/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <moxygen/compat/Config.h>

#if !MOXYGEN_USE_FOLLY

#include <moxygen/compat/ByteBuffer.h>
#include <moxygen/compat/ByteBufferQueue.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace moxygen::compat {

// Specific exception types for cursor operations (like Folly)
class CursorUnderflowError : public std::out_of_range {
 public:
  explicit CursorUnderflowError(const std::string& msg)
      : std::out_of_range(msg) {}
  explicit CursorUnderflowError(const char* msg) : std::out_of_range(msg) {}
};

class CursorOverflowError : public std::overflow_error {
 public:
  explicit CursorOverflowError(const std::string& msg)
      : std::overflow_error(msg) {}
  explicit CursorOverflowError(const char* msg) : std::overflow_error(msg) {}
};

class CursorBoundsError : public std::out_of_range {
 public:
  explicit CursorBoundsError(const std::string& msg)
      : std::out_of_range(msg) {}
  explicit CursorBoundsError(const char* msg) : std::out_of_range(msg) {}
};

// Runtime endianness detection
namespace detail {

inline bool isLittleEndian() {
  static const uint32_t test = 1;
  static const bool result = (*reinterpret_cast<const uint8_t*>(&test) == 1);
  return result;
}

// Use compiler intrinsics when available for optimal performance
inline uint16_t byteSwap16(uint16_t value) {
#if defined(__GNUC__) || defined(__clang__)
  return __builtin_bswap16(value);
#elif defined(_MSC_VER)
  return _byteswap_ushort(value);
#else
  return static_cast<uint16_t>((value >> 8) | (value << 8));
#endif
}

inline uint32_t byteSwap32(uint32_t value) {
#if defined(__GNUC__) || defined(__clang__)
  return __builtin_bswap32(value);
#elif defined(_MSC_VER)
  return _byteswap_ulong(value);
#else
  return ((value >> 24) & 0xFF) | ((value >> 8) & 0xFF00) |
         ((value << 8) & 0xFF0000) | ((value << 24) & 0xFF000000);
#endif
}

inline uint64_t byteSwap64(uint64_t value) {
#if defined(__GNUC__) || defined(__clang__)
  return __builtin_bswap64(value);
#elif defined(_MSC_VER)
  return _byteswap_uint64(value);
#else
  return ((value >> 56) & 0xFFULL) | ((value >> 40) & 0xFF00ULL) |
         ((value >> 24) & 0xFF0000ULL) | ((value >> 8) & 0xFF000000ULL) |
         ((value << 8) & 0xFF00000000ULL) |
         ((value << 24) & 0xFF0000000000ULL) |
         ((value << 40) & 0xFF000000000000ULL) |
         ((value << 56) & 0xFF00000000000000ULL);
#endif
}

template <typename T>
T hostToBigEndian(T value) {
  if constexpr (sizeof(T) == 1) {
    return value;
  } else if constexpr (sizeof(T) == 2) {
    if (isLittleEndian()) {
      return static_cast<T>(byteSwap16(static_cast<uint16_t>(value)));
    }
    return value;
  } else if constexpr (sizeof(T) == 4) {
    if (isLittleEndian()) {
      return static_cast<T>(byteSwap32(static_cast<uint32_t>(value)));
    }
    return value;
  } else if constexpr (sizeof(T) == 8) {
    if (isLittleEndian()) {
      return static_cast<T>(byteSwap64(static_cast<uint64_t>(value)));
    }
    return value;
  }
}

template <typename T>
T bigEndianToHost(T value) {
  return hostToBigEndian(value); // Same operation - swap is symmetric
}

template <typename T>
T hostToLittleEndian(T value) {
  if constexpr (sizeof(T) == 1) {
    return value;
  } else if constexpr (sizeof(T) == 2) {
    if (!isLittleEndian()) {
      return static_cast<T>(byteSwap16(static_cast<uint16_t>(value)));
    }
    return value;
  } else if constexpr (sizeof(T) == 4) {
    if (!isLittleEndian()) {
      return static_cast<T>(byteSwap32(static_cast<uint32_t>(value)));
    }
    return value;
  } else if constexpr (sizeof(T) == 8) {
    if (!isLittleEndian()) {
      return static_cast<T>(byteSwap64(static_cast<uint64_t>(value)));
    }
    return value;
  }
}

template <typename T>
T littleEndianToHost(T value) {
  return hostToLittleEndian(value); // Same operation
}

} // namespace detail

// ByteRange - lightweight view into contiguous memory (like folly::ByteRange)
// Used for zero-copy peekBytes() operations
class ByteRange {
 public:
  ByteRange() = default;
  ByteRange(const uint8_t* data, size_t size) : data_(data), size_(size) {}

  const uint8_t* data() const { return data_; }
  size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }

  const uint8_t* begin() const { return data_; }
  const uint8_t* end() const { return data_ + size_; }

  uint8_t operator[](size_t i) const { return data_[i]; }

  ByteRange subpiece(size_t start, size_t len = std::string::npos) const {
    if (start >= size_) {
      return ByteRange();
    }
    size_t actualLen = std::min(len, size_ - start);
    return ByteRange(data_ + start, actualLen);
  }

  std::string toString() const {
    return std::string(reinterpret_cast<const char*>(data_), size_);
  }

 private:
  const uint8_t* data_{nullptr};
  size_t size_{0};
};

// Std-mode replacement for folly::io::Cursor
// Reads from a ByteBufferQueue without consuming it
//
// Optimizations over basic implementation:
// - O(1) totalLength() via cached length
// - Zero-copy peekBytes() for contiguous access
// - Compiler intrinsics for endian conversion
// - Cross-buffer retreat() support
//
class ByteCursor {
 public:
  explicit ByteCursor(const ByteBufferQueue* queue)
      : queue_(queue), bufferIndex_(0), offsetInBuffer_(0) {
    cachedTotalLength_ = queue_ ? queue_->chainLength() : 0;
    updateCurrentBuffer();
  }

  explicit ByteCursor(const ByteBuffer* buf)
      : singleBuffer_(buf), bufferIndex_(0), offsetInBuffer_(0) {
    if (buf) {
      currentData_ = buf->data();
      currentLength_ = buf->length();
      cachedTotalLength_ = buf->length();
    } else {
      cachedTotalLength_ = 0;
    }
  }

  // Copy constructor - for creating checkpoints
  ByteCursor(const ByteCursor&) = default;
  ByteCursor& operator=(const ByteCursor&) = default;

  // Total remaining bytes - O(1) via cached length
  size_t totalLength() const {
    return cachedTotalLength_;
  }

  bool isAtEnd() const {
    return cachedTotalLength_ == 0;
  }

  // Length remaining in current buffer
  size_t length() const {
    return currentLength_ - offsetInBuffer_;
  }

  // Pointer to current position in current buffer
  const uint8_t* data() const {
    return currentData_ + offsetInBuffer_;
  }

  // Zero-copy peek at next n bytes (or less if not enough data)
  // Returns a view into the CURRENT buffer only - may return fewer bytes
  // than requested if data spans multiple buffers
  ByteRange peekBytes() const {
    return ByteRange(data(), length());
  }

  // Peek up to n bytes from current buffer (zero-copy)
  ByteRange peekBytes(size_t n) const {
    size_t available = length();
    return ByteRange(data(), std::min(n, available));
  }

  // Read a fixed-size integer (native endian)
  template <typename T>
  T read() {
    static_assert(std::is_arithmetic_v<T>, "T must be arithmetic");
    T value;
    pull(&value, sizeof(T));
    return value;
  }

  // Read a fixed-size integer (big endian)
  template <typename T>
  T readBE() {
    static_assert(std::is_integral_v<T>, "T must be integral");
    T value = read<T>();
    return detail::bigEndianToHost(value);
  }

  // Read a fixed-size integer (little endian)
  template <typename T>
  T readLE() {
    static_assert(std::is_integral_v<T>, "T must be integral");
    T value = read<T>();
    return detail::littleEndianToHost(value);
  }

  // Try to read (returns nullopt if not enough data)
  template <typename T>
  std::optional<T> tryRead() {
    if (cachedTotalLength_ < sizeof(T)) {
      return std::nullopt;
    }
    return read<T>();
  }

  template <typename T>
  std::optional<T> tryReadBE() {
    if (cachedTotalLength_ < sizeof(T)) {
      return std::nullopt;
    }
    return readBE<T>();
  }

  template <typename T>
  std::optional<T> tryReadLE() {
    if (cachedTotalLength_ < sizeof(T)) {
      return std::nullopt;
    }
    return readLE<T>();
  }

  // Skip bytes
  void skip(size_t n) {
    if (n > cachedTotalLength_) {
      throw CursorUnderflowError("ByteCursor::skip past end");
    }

    cachedTotalLength_ -= n;

    while (n > 0) {
      size_t available = currentLength_ - offsetInBuffer_;
      if (available == 0) {
        advanceBuffer();
        continue;
      }

      size_t toSkip = std::min(n, available);
      offsetInBuffer_ += toSkip;
      n -= toSkip;

      if (offsetInBuffer_ >= currentLength_ && n > 0) {
        advanceBuffer();
      }
    }
  }

  // Skip all remaining bytes
  void skipAtEnd() {
    skip(cachedTotalLength_);
  }

  // Try to skip (returns false if not enough data)
  bool trySkip(size_t n) {
    if (cachedTotalLength_ < n) {
      return false;
    }
    skip(n);
    return true;
  }

  // Pull bytes into a buffer
  void pull(void* buf, size_t len) {
    if (len > cachedTotalLength_) {
      throw CursorUnderflowError("ByteCursor::pull past end");
    }

    size_t pulled = pullAtMostImpl(buf, len);
    if (pulled != len) {
      throw CursorUnderflowError("ByteCursor::pull incomplete");
    }
  }

  // Pull up to len bytes, returns actual bytes pulled
  size_t pullAtMost(void* buf, size_t len) {
    return pullAtMostImpl(buf, std::min(len, cachedTotalLength_));
  }

  // Clone n bytes into a new ByteBuffer (throws if not enough data)
  std::unique_ptr<ByteBuffer> clone(size_t n) {
    if (n > cachedTotalLength_) {
      throw CursorUnderflowError("ByteCursor::clone past end");
    }
    auto result = ByteBuffer::create(n);
    pull(result->writableData(), n);
    result->append(n);
    return result;
  }

  // Clone up to n bytes (doesn't throw)
  std::unique_ptr<ByteBuffer> cloneAtMost(size_t n) {
    size_t toClone = std::min(n, cachedTotalLength_);
    if (toClone == 0) {
      return nullptr;
    }
    auto result = ByteBuffer::create(toClone);
    size_t pulled = pullAtMostImpl(result->writableData(), toClone);
    result->append(pulled);
    return result;
  }

  // Peek at next byte without consuming
  uint8_t peek() const {
    if (cachedTotalLength_ == 0) {
      throw CursorUnderflowError("ByteCursor::peek at end");
    }
    return currentData_[offsetInBuffer_];
  }

  std::optional<uint8_t> tryPeek() const {
    if (cachedTotalLength_ == 0) {
      return std::nullopt;
    }
    return currentData_[offsetInBuffer_];
  }

  // Read a fixed-length string
  std::string readFixedString(size_t len) {
    std::string result(len, '\0');
    pull(result.data(), len);
    return result;
  }

  // Try to read a fixed-length string
  std::optional<std::string> tryReadFixedString(size_t len) {
    if (cachedTotalLength_ < len) {
      return std::nullopt;
    }
    return readFixedString(len);
  }

  // Read until null terminator (like Folly's readTerminatedString)
  std::string readTerminatedString(char terminator = '\0', size_t maxLength = std::string::npos) {
    std::string result;
    size_t count = 0;
    size_t limit = std::min(maxLength, cachedTotalLength_);

    while (count < limit) {
      if (length() == 0) {
        if (!advanceBufferConst()) {
          break;
        }
      }

      uint8_t byte = currentData_[offsetInBuffer_];
      if (static_cast<char>(byte) == terminator) {
        // Skip the terminator
        skip(1);
        return result;
      }

      result.push_back(static_cast<char>(byte));
      skip(1);
      ++count;
    }

    // No terminator found within limit
    throw CursorUnderflowError("ByteCursor::readTerminatedString: terminator not found");
  }

  // Try to read until null terminator (returns nullopt if terminator not found)
  std::optional<std::string> tryReadTerminatedString(
      char terminator = '\0',
      size_t maxLength = std::string::npos) {
    // Save cursor state for rollback
    ByteCursor saved = *this;

    std::string result;
    size_t count = 0;
    size_t limit = std::min(maxLength, cachedTotalLength_);

    while (count < limit) {
      if (length() == 0) {
        if (!advanceBufferConst()) {
          break;
        }
      }

      if (cachedTotalLength_ == 0) {
        break;
      }

      uint8_t byte = currentData_[offsetInBuffer_];
      if (static_cast<char>(byte) == terminator) {
        // Skip the terminator
        skip(1);
        return result;
      }

      result.push_back(static_cast<char>(byte));
      skip(1);
      ++count;
    }

    // No terminator found - rollback
    *this = saved;
    return std::nullopt;
  }

  // Read null-terminated string (convenience wrapper)
  std::string readNullTerminatedString(size_t maxLength = std::string::npos) {
    return readTerminatedString('\0', maxLength);
  }

  // Read until whitespace
  std::string readWhileNot(char stopChar, size_t maxLength = std::string::npos) {
    std::string result;
    size_t count = 0;
    size_t limit = std::min(maxLength, cachedTotalLength_);

    while (count < limit && cachedTotalLength_ > 0) {
      uint8_t byte = peek();
      if (static_cast<char>(byte) == stopChar) {
        return result;
      }
      result.push_back(static_cast<char>(byte));
      skip(1);
      ++count;
    }

    return result;
  }

  // Retreat cursor position (supports cross-buffer retreat)
  void retreat(size_t n) {
    if (singleBuffer_) {
      // Single buffer case
      if (n > offsetInBuffer_) {
        throw CursorOverflowError("ByteCursor::retreat past beginning");
      }
      offsetInBuffer_ -= n;
      cachedTotalLength_ += n;
      return;
    }

    if (!queue_) {
      throw CursorOverflowError("ByteCursor::retreat on null queue");
    }

    // Calculate total consumed so far
    size_t totalConsumed = 0;
    const auto& chain = queue_->chain();
    for (size_t i = 0; i < bufferIndex_; ++i) {
      totalConsumed += chain[i]->length();
    }
    totalConsumed += offsetInBuffer_;

    if (n > totalConsumed) {
      throw CursorOverflowError("ByteCursor::retreat past beginning");
    }

    // Update cached length
    cachedTotalLength_ += n;

    // Simple case: retreat within current buffer
    if (n <= offsetInBuffer_) {
      offsetInBuffer_ -= n;
      return;
    }

    // Cross-buffer retreat
    n -= offsetInBuffer_;
    while (n > 0 && bufferIndex_ > 0) {
      --bufferIndex_;
      size_t bufLen = chain[bufferIndex_]->length();
      if (n <= bufLen) {
        offsetInBuffer_ = bufLen - n;
        currentData_ = chain[bufferIndex_]->data();
        currentLength_ = bufLen;
        return;
      }
      n -= bufLen;
    }

    // Should have retreated to beginning
    offsetInBuffer_ = 0;
    if (bufferIndex_ < chain.size()) {
      currentData_ = chain[bufferIndex_]->data();
      currentLength_ = chain[bufferIndex_]->length();
    }
  }

  // Check if we can read n bytes - O(1)
  bool canAdvance(size_t n) const {
    return cachedTotalLength_ >= n;
  }

  // Cursor difference (bytes between two cursors on same queue)
  size_t operator-(const ByteCursor& other) const {
    // This cursor should be ahead of other
    return other.cachedTotalLength_ - cachedTotalLength_;
  }

 private:
  size_t pullAtMostImpl(void* buf, size_t len) {
    uint8_t* dest = static_cast<uint8_t*>(buf);
    size_t totalPulled = 0;

    while (len > 0) {
      size_t available = currentLength_ - offsetInBuffer_;
      if (available == 0) {
        if (!advanceBuffer()) {
          break;
        }
        continue;
      }

      size_t toCopy = std::min(len, available);
      std::memcpy(dest, currentData_ + offsetInBuffer_, toCopy);

      dest += toCopy;
      len -= toCopy;
      totalPulled += toCopy;
      offsetInBuffer_ += toCopy;
      cachedTotalLength_ -= toCopy;

      if (offsetInBuffer_ >= currentLength_ && len > 0) {
        advanceBuffer();
      }
    }

    return totalPulled;
  }

  void updateCurrentBuffer() {
    if (queue_) {
      const auto& chain = queue_->chain();
      if (bufferIndex_ < chain.size()) {
        currentData_ = chain[bufferIndex_]->data();
        currentLength_ = chain[bufferIndex_]->length();
      } else {
        currentData_ = nullptr;
        currentLength_ = 0;
      }
    }
  }

  bool advanceBuffer() {
    if (singleBuffer_) {
      currentData_ = nullptr;
      currentLength_ = 0;
      return false;
    }

    if (!queue_) {
      return false;
    }

    const auto& chain = queue_->chain();
    ++bufferIndex_;
    offsetInBuffer_ = 0;

    if (bufferIndex_ < chain.size()) {
      currentData_ = chain[bufferIndex_]->data();
      currentLength_ = chain[bufferIndex_]->length();
      return true;
    }

    currentData_ = nullptr;
    currentLength_ = 0;
    return false;
  }

  // Const version for use in string reading (updates internal state)
  bool advanceBufferConst() {
    return const_cast<ByteCursor*>(this)->advanceBuffer();
  }

  const ByteBufferQueue* queue_{nullptr};
  const ByteBuffer* singleBuffer_{nullptr};
  const uint8_t* currentData_{nullptr};
  size_t currentLength_{0};
  size_t bufferIndex_{0};
  size_t offsetInBuffer_{0};
  size_t cachedTotalLength_{0}; // O(1) totalLength()
};

// BoundedCursor - cursor with enforced length limit for safe parsing
// Use this when parsing untrusted data to prevent reading past expected bounds
class BoundedCursor {
 public:
  BoundedCursor(ByteCursor cursor, size_t maxLength)
      : cursor_(std::move(cursor)),
        maxLength_(std::min(maxLength, cursor_.totalLength())),
        consumed_(0) {}

  // Remaining bytes within bounds
  size_t totalLength() const {
    return maxLength_ - consumed_;
  }

  bool isAtEnd() const {
    return consumed_ >= maxLength_;
  }

  size_t length() const {
    return std::min(cursor_.length(), totalLength());
  }

  const uint8_t* data() const {
    return cursor_.data();
  }

  ByteRange peekBytes() const {
    auto range = cursor_.peekBytes();
    return ByteRange(range.data(), std::min(range.size(), totalLength()));
  }

  ByteRange peekBytes(size_t n) const {
    return cursor_.peekBytes(std::min(n, totalLength()));
  }

  template <typename T>
  T read() {
    if (sizeof(T) > totalLength()) {
      throw CursorBoundsError("BoundedCursor::read past bounds");
    }
    T value = cursor_.read<T>();
    consumed_ += sizeof(T);
    return value;
  }

  template <typename T>
  T readBE() {
    if (sizeof(T) > totalLength()) {
      throw CursorBoundsError("BoundedCursor::readBE past bounds");
    }
    T value = cursor_.readBE<T>();
    consumed_ += sizeof(T);
    return value;
  }

  template <typename T>
  T readLE() {
    if (sizeof(T) > totalLength()) {
      throw CursorBoundsError("BoundedCursor::readLE past bounds");
    }
    T value = cursor_.readLE<T>();
    consumed_ += sizeof(T);
    return value;
  }

  template <typename T>
  std::optional<T> tryRead() {
    if (sizeof(T) > totalLength()) {
      return std::nullopt;
    }
    return read<T>();
  }

  template <typename T>
  std::optional<T> tryReadBE() {
    if (sizeof(T) > totalLength()) {
      return std::nullopt;
    }
    return readBE<T>();
  }

  template <typename T>
  std::optional<T> tryReadLE() {
    if (sizeof(T) > totalLength()) {
      return std::nullopt;
    }
    return readLE<T>();
  }

  void skip(size_t n) {
    if (n > totalLength()) {
      throw CursorBoundsError("BoundedCursor::skip past bounds");
    }
    cursor_.skip(n);
    consumed_ += n;
  }

  bool trySkip(size_t n) {
    if (n > totalLength()) {
      return false;
    }
    skip(n);
    return true;
  }

  void pull(void* buf, size_t len) {
    if (len > totalLength()) {
      throw CursorBoundsError("BoundedCursor::pull past bounds");
    }
    cursor_.pull(buf, len);
    consumed_ += len;
  }

  size_t pullAtMost(void* buf, size_t len) {
    size_t toPull = std::min(len, totalLength());
    size_t pulled = cursor_.pullAtMost(buf, toPull);
    consumed_ += pulled;
    return pulled;
  }

  std::unique_ptr<ByteBuffer> clone(size_t n) {
    if (n > totalLength()) {
      throw CursorBoundsError("BoundedCursor::clone past bounds");
    }
    auto result = cursor_.clone(n);
    consumed_ += n;
    return result;
  }

  std::unique_ptr<ByteBuffer> cloneAtMost(size_t n) {
    size_t toClone = std::min(n, totalLength());
    auto result = cursor_.cloneAtMost(toClone);
    if (result) {
      consumed_ += result->length();
    }
    return result;
  }

  uint8_t peek() const {
    if (totalLength() == 0) {
      throw CursorBoundsError("BoundedCursor::peek at end");
    }
    return cursor_.peek();
  }

  std::optional<uint8_t> tryPeek() const {
    if (totalLength() == 0) {
      return std::nullopt;
    }
    return cursor_.tryPeek();
  }

  std::string readFixedString(size_t len) {
    if (len > totalLength()) {
      throw CursorBoundsError("BoundedCursor::readFixedString past bounds");
    }
    std::string result = cursor_.readFixedString(len);
    consumed_ += len;
    return result;
  }

  std::optional<std::string> tryReadFixedString(size_t len) {
    if (len > totalLength()) {
      return std::nullopt;
    }
    return readFixedString(len);
  }

  bool canAdvance(size_t n) const {
    return totalLength() >= n;
  }

  // Access to underlying cursor (for operations that don't advance)
  const ByteCursor& cursor() const {
    return cursor_;
  }

 private:
  ByteCursor cursor_;
  size_t maxLength_;
  size_t consumed_;
};

// RWCursor - Read-Write cursor for in-place modification (like Folly's RWCursor)
// Allows reading and writing to an existing buffer without copying
class RWCursor {
 public:
  explicit RWCursor(ByteBuffer* buf)
      : buf_(buf), offset_(0) {
    if (buf_) {
      length_ = buf_->length();
    }
  }

  // Read operations (same as ByteCursor)
  size_t totalLength() const {
    return length_ - offset_;
  }

  bool isAtEnd() const {
    return offset_ >= length_;
  }

  size_t length() const {
    return length_ - offset_;
  }

  const uint8_t* data() const {
    return buf_ ? buf_->data() + offset_ : nullptr;
  }

  uint8_t* writableData() {
    return buf_ ? buf_->writableData() + offset_ : nullptr;
  }

  template <typename T>
  T read() {
    static_assert(std::is_arithmetic_v<T>, "T must be arithmetic");
    if (sizeof(T) > totalLength()) {
      throw CursorUnderflowError("RWCursor::read past end");
    }
    T value;
    std::memcpy(&value, data(), sizeof(T));
    offset_ += sizeof(T);
    return value;
  }

  template <typename T>
  T readBE() {
    static_assert(std::is_integral_v<T>, "T must be integral");
    T value = read<T>();
    return detail::bigEndianToHost(value);
  }

  template <typename T>
  T readLE() {
    static_assert(std::is_integral_v<T>, "T must be integral");
    T value = read<T>();
    return detail::littleEndianToHost(value);
  }

  // Write operations - modify buffer in-place
  template <typename T>
  void write(T value) {
    static_assert(std::is_arithmetic_v<T>, "T must be arithmetic");
    if (sizeof(T) > totalLength()) {
      throw CursorOverflowError("RWCursor::write past end");
    }
    std::memcpy(writableData(), &value, sizeof(T));
    offset_ += sizeof(T);
  }

  template <typename T>
  void writeBE(T value) {
    static_assert(std::is_integral_v<T>, "T must be integral");
    T be = detail::hostToBigEndian(value);
    write(be);
  }

  template <typename T>
  void writeLE(T value) {
    static_assert(std::is_integral_v<T>, "T must be integral");
    T le = detail::hostToLittleEndian(value);
    write(le);
  }

  void write(const void* src, size_t len) {
    if (len > totalLength()) {
      throw CursorOverflowError("RWCursor::write past end");
    }
    std::memcpy(writableData(), src, len);
    offset_ += len;
  }

  void skip(size_t n) {
    if (n > totalLength()) {
      throw CursorUnderflowError("RWCursor::skip past end");
    }
    offset_ += n;
  }

  void retreat(size_t n) {
    if (n > offset_) {
      throw CursorOverflowError("RWCursor::retreat past beginning");
    }
    offset_ -= n;
  }

  // Get current position
  size_t position() const {
    return offset_;
  }

  // Seek to absolute position
  void seek(size_t pos) {
    if (pos > length_) {
      throw CursorOverflowError("RWCursor::seek past end");
    }
    offset_ = pos;
  }

  // Reset to beginning
  void reset() {
    offset_ = 0;
  }

 private:
  ByteBuffer* buf_{nullptr};
  size_t offset_{0};
  size_t length_{0};
};

// RWPrivateCursor - RWCursor that takes ownership of buffer
// Use when you want to modify a buffer and ensure exclusive access
class RWPrivateCursor {
 public:
  explicit RWPrivateCursor(std::unique_ptr<ByteBuffer> buf)
      : buf_(std::move(buf)), cursor_(buf_.get()) {}

  // Forward all operations to internal RWCursor
  size_t totalLength() const { return cursor_.totalLength(); }
  bool isAtEnd() const { return cursor_.isAtEnd(); }
  size_t length() const { return cursor_.length(); }
  const uint8_t* data() const { return cursor_.data(); }
  uint8_t* writableData() { return cursor_.writableData(); }

  template <typename T>
  T read() { return cursor_.read<T>(); }

  template <typename T>
  T readBE() { return cursor_.readBE<T>(); }

  template <typename T>
  T readLE() { return cursor_.readLE<T>(); }

  template <typename T>
  void write(T value) { cursor_.write(value); }

  template <typename T>
  void writeBE(T value) { cursor_.writeBE(value); }

  template <typename T>
  void writeLE(T value) { cursor_.writeLE(value); }

  void write(const void* src, size_t len) { cursor_.write(src, len); }
  void skip(size_t n) { cursor_.skip(n); }
  void retreat(size_t n) { cursor_.retreat(n); }
  size_t position() const { return cursor_.position(); }
  void seek(size_t pos) { cursor_.seek(pos); }
  void reset() { cursor_.reset(); }

  // Release ownership of buffer
  std::unique_ptr<ByteBuffer> release() {
    return std::move(buf_);
  }

  // Access underlying buffer
  ByteBuffer* buffer() { return buf_.get(); }
  const ByteBuffer* buffer() const { return buf_.get(); }

 private:
  std::unique_ptr<ByteBuffer> buf_;
  RWCursor cursor_;
};

// Write cursor for building buffers
class ByteAppender {
 public:
  explicit ByteAppender(ByteBufferQueue* queue, size_t growth = 4096)
      : queue_(queue), growth_(growth) {}

  void write(const void* data, size_t len) {
    auto buf = ByteBuffer::copyBuffer(data, len);
    queue_->append(std::move(buf));
  }

  template <typename T>
  void writeBE(T value) {
    static_assert(std::is_integral_v<T>, "T must be integral");
    T be = detail::hostToBigEndian(value);
    write(&be, sizeof(T));
  }

  template <typename T>
  void writeLE(T value) {
    static_assert(std::is_integral_v<T>, "T must be integral");
    T le = detail::hostToLittleEndian(value);
    write(&le, sizeof(T));
  }

  void push(const uint8_t* data, size_t len) {
    write(data, len);
  }

  // Get growth hint (may be used for buffer preallocation)
  size_t growth() const {
    return growth_;
  }

 private:
  ByteBufferQueue* queue_;
  size_t growth_;
};

} // namespace moxygen::compat

#endif // !MOXYGEN_USE_FOLLY
