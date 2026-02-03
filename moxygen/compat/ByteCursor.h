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
#include <type_traits>

namespace moxygen::compat {

// Std-mode replacement for folly::io::Cursor
// Reads from a ByteBufferQueue without consuming it
class ByteCursor {
 public:
  explicit ByteCursor(const ByteBufferQueue* queue)
      : queue_(queue), bufferIndex_(0), offsetInBuffer_(0) {
    updateCurrentBuffer();
  }

  explicit ByteCursor(const ByteBuffer* buf)
      : singleBuffer_(buf), bufferIndex_(0), offsetInBuffer_(0) {
    if (buf) {
      currentData_ = buf->data();
      currentLength_ = buf->length();
    }
  }

  // Total remaining bytes
  size_t totalLength() const {
    if (singleBuffer_) {
      return currentLength_ - offsetInBuffer_;
    }

    size_t total = 0;
    if (queue_) {
      const auto& chain = queue_->chain();
      for (size_t i = bufferIndex_; i < chain.size(); ++i) {
        if (i == bufferIndex_) {
          total += chain[i]->length() - offsetInBuffer_;
        } else {
          total += chain[i]->length();
        }
      }
    }
    return total;
  }

  bool isAtEnd() const {
    return totalLength() == 0;
  }

  size_t length() const {
    return currentLength_ - offsetInBuffer_;
  }

  const uint8_t* data() const {
    return currentData_ + offsetInBuffer_;
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
    return toBigEndian(value);
  }

  // Read a fixed-size integer (little endian)
  template <typename T>
  T readLE() {
    static_assert(std::is_integral_v<T>, "T must be integral");
    return read<T>(); // Assuming little-endian host
  }

  // Try to read (returns nullopt if not enough data)
  template <typename T>
  std::optional<T> tryRead() {
    if (totalLength() < sizeof(T)) {
      return std::nullopt;
    }
    return read<T>();
  }

  template <typename T>
  std::optional<T> tryReadBE() {
    if (totalLength() < sizeof(T)) {
      return std::nullopt;
    }
    return readBE<T>();
  }

  // Skip bytes
  void skip(size_t n) {
    while (n > 0) {
      size_t available = currentLength_ - offsetInBuffer_;
      if (available == 0) {
        if (!advanceBuffer()) {
          throw std::out_of_range("ByteCursor::skip past end");
        }
        continue;
      }

      size_t toSkip = std::min(n, available);
      offsetInBuffer_ += toSkip;
      n -= toSkip;

      if (offsetInBuffer_ >= currentLength_) {
        advanceBuffer();
      }
    }
  }

  // Try to skip (returns false if not enough data)
  bool trySkip(size_t n) {
    if (totalLength() < n) {
      return false;
    }
    skip(n);
    return true;
  }

  // Pull bytes into a buffer
  void pull(void* buf, size_t len) {
    size_t pulled = pullAtMost(buf, len);
    if (pulled != len) {
      throw std::out_of_range("ByteCursor::pull past end");
    }
  }

  // Pull up to len bytes, returns actual bytes pulled
  size_t pullAtMost(void* buf, size_t len) {
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

      if (offsetInBuffer_ >= currentLength_) {
        advanceBuffer();
      }
    }

    return totalPulled;
  }

  // Clone n bytes into a new ByteBuffer
  std::unique_ptr<ByteBuffer> clone(size_t n) {
    auto result = ByteBuffer::create(n);
    size_t pulled = pullAtMost(result->writableData(), n);
    result->append(pulled);
    return result;
  }

  // Peek at next byte without consuming
  uint8_t peek() const {
    if (totalLength() == 0) {
      throw std::out_of_range("ByteCursor::peek at end");
    }
    return currentData_[offsetInBuffer_];
  }

  std::optional<uint8_t> tryPeek() const {
    if (totalLength() == 0) {
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

  // Try to read a fixed-length string (returns empty string on underflow)
  std::string tryReadFixedString(size_t len) {
    if (totalLength() < len) {
      return {};
    }
    return readFixedString(len);
  }

  // Retreat cursor position (for re-reading)
  void retreat(size_t n) {
    // Simple implementation for single buffer case
    if (n <= offsetInBuffer_) {
      offsetInBuffer_ -= n;
    } else {
      throw std::out_of_range("ByteCursor::retreat past beginning");
    }
  }

  // Check if we can read n bytes
  bool canAdvance(size_t n) const {
    return totalLength() >= n;
  }

 private:
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

  template <typename T>
  static T toBigEndian(T value) {
    // Convert from native (assumed little-endian) to big-endian
    if constexpr (sizeof(T) == 1) {
      return value;
    } else if constexpr (sizeof(T) == 2) {
      return static_cast<T>((value >> 8) | (value << 8));
    } else if constexpr (sizeof(T) == 4) {
      return static_cast<T>(
          ((value >> 24) & 0xFF) | ((value >> 8) & 0xFF00) |
          ((value << 8) & 0xFF0000) | ((value << 24) & 0xFF000000));
    } else if constexpr (sizeof(T) == 8) {
      return static_cast<T>(
          ((value >> 56) & 0xFF) | ((value >> 40) & 0xFF00) |
          ((value >> 24) & 0xFF0000) | ((value >> 8) & 0xFF000000) |
          ((value << 8) & 0xFF00000000) | ((value << 24) & 0xFF0000000000) |
          ((value << 40) & 0xFF000000000000) |
          ((value << 56) & 0xFF00000000000000));
    }
  }

  const ByteBufferQueue* queue_{nullptr};
  const ByteBuffer* singleBuffer_{nullptr};
  const uint8_t* currentData_{nullptr};
  size_t currentLength_{0};
  size_t bufferIndex_{0};
  size_t offsetInBuffer_{0};
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
    T be = toBigEndian(value);
    write(&be, sizeof(T));
  }

  void push(const uint8_t* data, size_t len) {
    write(data, len);
  }

 private:
  template <typename T>
  static T toBigEndian(T value) {
    if constexpr (sizeof(T) == 1) {
      return value;
    } else if constexpr (sizeof(T) == 2) {
      return static_cast<T>((value >> 8) | (value << 8));
    } else if constexpr (sizeof(T) == 4) {
      return static_cast<T>(
          ((value >> 24) & 0xFF) | ((value >> 8) & 0xFF00) |
          ((value << 8) & 0xFF0000) | ((value << 24) & 0xFF000000));
    } else if constexpr (sizeof(T) == 8) {
      return static_cast<T>(
          ((value >> 56) & 0xFF) | ((value >> 40) & 0xFF00) |
          ((value >> 24) & 0xFF0000) | ((value >> 8) & 0xFF000000) |
          ((value << 8) & 0xFF00000000) | ((value << 24) & 0xFF0000000000) |
          ((value << 40) & 0xFF000000000000) |
          ((value << 56) & 0xFF00000000000000));
    }
  }

  ByteBufferQueue* queue_;
  size_t growth_;
};

} // namespace moxygen::compat

#endif // !MOXYGEN_USE_FOLLY
