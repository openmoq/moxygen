/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <moxygen/compat/Config.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>

namespace moxygen::compat {

#if MOXYGEN_USE_FOLLY

// In Folly mode, ByteBuffer is not used directly - use IOBuf via Payload
// This header provides the std-mode implementation only

#else // !MOXYGEN_USE_FOLLY

// Std-mode ByteBuffer - IOBuf-like buffer with headroom/tailroom and SBO
//
// Features:
// - O(1) trimStart() via offset adjustment (like IOBuf headroom)
// - O(1) prepend() when headroom available
// - Small Buffer Optimization: buffers <= 64 bytes use inline storage
// - Overflow-safe arithmetic in append/prepend
//
// Memory layout (heap mode):
// |<-- headroom -->|<------- length -------->|<--- tailroom --->|
// |   (offset_)    |      (valid data)       |                  |
// +----------------+-------------------------+------------------+
// ^                ^                         ^                  ^
// heap_.data    data()              data()+length()    heap_.data+capacity
//
class ByteBuffer {
 public:
  // Configuration constants
  static constexpr size_t kInlineSize = 64;        // SBO threshold
  static constexpr size_t kDefaultHeadroom = 128;  // Pre-allocated headroom
  static constexpr size_t kDefaultTailroom = 256;  // Pre-allocated tailroom

  // Default constructor - inline mode, empty
  ByteBuffer() : isInline_(true), offset_(0), length_(0) {
    // Zero-initialize inline storage
    std::memset(storage_.inline_.data(), 0, kInlineSize);
  }

  // Construct with capacity - chooses inline vs heap based on size
  explicit ByteBuffer(size_t capacity)
      : isInline_(capacity <= kInlineSize), offset_(0), length_(0) {
    if (isInline_) {
      std::memset(storage_.inline_.data(), 0, kInlineSize);
    } else {
      // Allocate with headroom for future prepends
      size_t allocSize = capacity + kDefaultHeadroom;
      storage_.heap_.data = new uint8_t[allocSize];
      storage_.heap_.capacity = allocSize;
      offset_ = kDefaultHeadroom;  // Start with headroom
    }
  }

  // Destructor - clean up heap allocation
  ~ByteBuffer() {
    if (!isInline_ && storage_.heap_.data) {
      delete[] storage_.heap_.data;
    }
  }

  // Move constructor
  ByteBuffer(ByteBuffer&& other) noexcept
      : isInline_(other.isInline_), offset_(other.offset_), length_(other.length_) {
    if (isInline_) {
      storage_.inline_ = other.storage_.inline_;
    } else {
      storage_.heap_ = other.storage_.heap_;
      other.storage_.heap_.data = nullptr;
      other.storage_.heap_.capacity = 0;
    }
    other.offset_ = 0;
    other.length_ = 0;
    other.isInline_ = true;
  }

  // Move assignment
  ByteBuffer& operator=(ByteBuffer&& other) noexcept {
    if (this != &other) {
      // Clean up current heap allocation
      if (!isInline_ && storage_.heap_.data) {
        delete[] storage_.heap_.data;
      }

      isInline_ = other.isInline_;
      offset_ = other.offset_;
      length_ = other.length_;

      if (isInline_) {
        storage_.inline_ = other.storage_.inline_;
      } else {
        storage_.heap_ = other.storage_.heap_;
        other.storage_.heap_.data = nullptr;
        other.storage_.heap_.capacity = 0;
      }

      other.offset_ = 0;
      other.length_ = 0;
      other.isInline_ = true;
    }
    return *this;
  }

  // Non-copyable
  ByteBuffer(const ByteBuffer&) = delete;
  ByteBuffer& operator=(const ByteBuffer&) = delete;

  // Factory methods matching IOBuf API
  static std::unique_ptr<ByteBuffer> create(size_t capacity) {
    return std::make_unique<ByteBuffer>(capacity);
  }

  static std::unique_ptr<ByteBuffer> copyBuffer(const void* data, size_t size) {
    auto buf = std::make_unique<ByteBuffer>(size);
    std::memcpy(buf->writableData(), data, size);
    buf->length_ = size;
    return buf;
  }

  static std::unique_ptr<ByteBuffer> copyBuffer(const std::string& str) {
    return copyBuffer(str.data(), str.size());
  }

  // Data access - accounts for offset
  uint8_t* writableData() {
    if (isInline_) {
      return storage_.inline_.data() + offset_;
    }
    return storage_.heap_.data + offset_;
  }

  const uint8_t* data() const {
    if (isInline_) {
      return storage_.inline_.data() + offset_;
    }
    return storage_.heap_.data + offset_;
  }

  size_t length() const {
    return length_;
  }

  size_t capacity() const {
    if (isInline_) {
      return kInlineSize - offset_;
    }
    return storage_.heap_.capacity - offset_;
  }

  bool empty() const {
    return length_ == 0;
  }

  // New: headroom/tailroom accessors
  size_t headroom() const {
    return offset_;
  }

  size_t tailroom() const {
    if (isInline_) {
      return kInlineSize - offset_ - length_;
    }
    return storage_.heap_.capacity - offset_ - length_;
  }

  // For compatibility with IOBuf chain operations (single buffer, no chain)
  size_t computeChainDataLength() const {
    return length_;
  }

  // Append - extend valid data length (with overflow check)
  void append(size_t len) {
    size_t newLength;
    if (__builtin_add_overflow(length_, len, &newLength)) {
      throw std::overflow_error("ByteBuffer::append overflow");
    }
    // Cap at available tailroom
    length_ = std::min(newLength, length_ + tailroom());
  }

  // Prepend - O(1) when headroom available, else reallocate
  void prepend(size_t len) {
    if (len == 0) return;

    // Overflow check
    size_t newLength;
    if (__builtin_add_overflow(length_, len, &newLength)) {
      throw std::overflow_error("ByteBuffer::prepend overflow");
    }

    if (len <= offset_) {
      // Have headroom - O(1)
      offset_ -= len;
      length_ = newLength;
    } else {
      // Need to reallocate with more headroom
      reallocateWithHeadroom(len);
    }
  }

  // TrimStart - O(1) via offset adjustment (the key optimization!)
  void trimStart(size_t len) {
    if (len >= length_) {
      // Trim everything - reset to maximize headroom
      offset_ += length_;
      length_ = 0;
    } else {
      offset_ += len;  // O(1)!
      length_ -= len;
    }
  }

  // TrimEnd - O(1), just reduce length
  void trimEnd(size_t len) {
    if (len >= length_) {
      length_ = 0;
    } else {
      length_ -= len;
    }
  }

  // Clone - deep copy
  std::unique_ptr<ByteBuffer> clone() const {
    auto copy = std::make_unique<ByteBuffer>(length_);
    std::memcpy(copy->writableData(), data(), length_);
    copy->length_ = length_;
    return copy;
  }

  // Coalesce - no-op for contiguous buffer
  void coalesce() {}

  // Reserve more capacity (may reallocate)
  void reserve(size_t newCapacity) {
    if (newCapacity > capacity()) {
      reallocateForCapacity(newCapacity);
    }
  }

  // Ensure writable space at end
  void ensureWritableSpace(size_t len) {
    if (len > tailroom()) {
      reallocateForCapacity(length_ + len);
    }
  }

  // String conversion
  std::string toString() const {
    return std::string(reinterpret_cast<const char*>(data()), length_);
  }

  std::string moveToString() {
    return toString();
  }

  // Shim class for Folly API compatibility
  // Allows code to call buf->moveToFbString().toStdString()
  class FbStringShim {
   public:
    explicit FbStringShim(std::string s) : str_(std::move(s)) {}
    std::string toStdString() const { return str_; }

   private:
    std::string str_;
  };

  FbStringShim moveToFbString() {
    return FbStringShim(toString());
  }

 private:
  // Storage union for SBO
  union Storage {
    std::array<uint8_t, kInlineSize> inline_;
    struct HeapData {
      uint8_t* data;
      size_t capacity;
    } heap_;

    // Constructors needed for union
    Storage() : inline_() {}
    ~Storage() {}  // Destructor handled by ByteBuffer
  } storage_;

  bool isInline_{true};
  size_t offset_{0};   // Headroom: valid data starts at offset_
  size_t length_{0};   // Valid data length

  // Reallocate with additional headroom for prepend
  void reallocateWithHeadroom(size_t additionalHeadroom) {
    size_t neededHeadroom = additionalHeadroom + kDefaultHeadroom;
    size_t newCapacity = neededHeadroom + length_ + kDefaultTailroom;

    uint8_t* newData = new uint8_t[newCapacity];
    size_t newOffset = neededHeadroom;

    // Copy existing data to new location
    std::memcpy(newData + newOffset, data(), length_);

    // Clean up old heap if needed
    if (!isInline_ && storage_.heap_.data) {
      delete[] storage_.heap_.data;
    }

    storage_.heap_.data = newData;
    storage_.heap_.capacity = newCapacity;
    offset_ = newOffset;
    isInline_ = false;
  }

  // Reallocate for more capacity (tailroom)
  void reallocateForCapacity(size_t newCapacity) {
    size_t allocSize = offset_ + newCapacity + kDefaultTailroom;

    uint8_t* newData = new uint8_t[allocSize];

    // Copy existing data preserving offset
    if (length_ > 0) {
      std::memcpy(newData + offset_, data(), length_);
    }

    // Clean up old heap if needed
    if (!isInline_ && storage_.heap_.data) {
      delete[] storage_.heap_.data;
    }

    storage_.heap_.data = newData;
    storage_.heap_.capacity = allocSize;
    isInline_ = false;
  }
};

#endif // MOXYGEN_USE_FOLLY

} // namespace moxygen::compat
