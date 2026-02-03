/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <moxygen/compat/Config.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace moxygen::compat {

#if MOXYGEN_USE_FOLLY

// In Folly mode, ByteBuffer is not used directly - use IOBuf via Payload
// This header provides the std-mode implementation only

#else // !MOXYGEN_USE_FOLLY

// Std-mode ByteBuffer - simple contiguous buffer replacement for IOBuf
class ByteBuffer {
 public:
  ByteBuffer() = default;

  explicit ByteBuffer(size_t capacity) : data_(capacity), length_(0) {}

  explicit ByteBuffer(std::vector<uint8_t> data)
      : data_(std::move(data)), length_(data_.size()) {}

  // Move-only
  ByteBuffer(ByteBuffer&&) = default;
  ByteBuffer& operator=(ByteBuffer&&) = default;
  ByteBuffer(const ByteBuffer&) = delete;
  ByteBuffer& operator=(const ByteBuffer&) = delete;

  // Factory methods matching IOBuf API
  static std::unique_ptr<ByteBuffer> create(size_t capacity) {
    return std::make_unique<ByteBuffer>(capacity);
  }

  static std::unique_ptr<ByteBuffer> copyBuffer(
      const void* data,
      size_t size) {
    auto buf = std::make_unique<ByteBuffer>(size);
    std::memcpy(buf->data_.data(), data, size);
    buf->length_ = size;
    return buf;
  }

  static std::unique_ptr<ByteBuffer> copyBuffer(const std::string& str) {
    return copyBuffer(str.data(), str.size());
  }

  // Data access
  uint8_t* writableData() {
    return data_.data();
  }

  const uint8_t* data() const {
    return data_.data();
  }

  size_t length() const {
    return length_;
  }

  size_t capacity() const {
    return data_.size();
  }

  bool empty() const {
    return length_ == 0;
  }

  // For compatibility with IOBuf chain operations (single buffer, no chain)
  size_t computeChainDataLength() const {
    return length_;
  }

  // Modify length (after writing to writableData)
  void append(size_t len) {
    length_ = std::min(length_ + len, data_.size());
  }

  void prepend(size_t len) {
    // For simplicity, prepend shifts data. Real IOBuf uses headroom.
    if (len > 0 && len <= data_.size() - length_) {
      std::memmove(data_.data() + len, data_.data(), length_);
      length_ += len;
    }
  }

  void trimStart(size_t len) {
    if (len >= length_) {
      length_ = 0;
    } else {
      std::memmove(data_.data(), data_.data() + len, length_ - len);
      length_ -= len;
    }
  }

  void trimEnd(size_t len) {
    if (len >= length_) {
      length_ = 0;
    } else {
      length_ -= len;
    }
  }

  // Clone
  std::unique_ptr<ByteBuffer> clone() const {
    auto copy = std::make_unique<ByteBuffer>(length_);
    std::memcpy(copy->data_.data(), data_.data(), length_);
    copy->length_ = length_;
    return copy;
  }

  // Coalesce (no-op for contiguous buffer)
  void coalesce() {}

  // Reserve more capacity
  void reserve(size_t newCapacity) {
    if (newCapacity > data_.size()) {
      data_.resize(newCapacity);
    }
  }

  // Ensure writable space at end
  void ensureWritableSpace(size_t len) {
    if (length_ + len > data_.size()) {
      data_.resize(length_ + len);
    }
  }

  // String conversion
  std::string toString() const {
    return std::string(reinterpret_cast<const char*>(data_.data()), length_);
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
  std::vector<uint8_t> data_;
  size_t length_{0};
};

#endif // MOXYGEN_USE_FOLLY

} // namespace moxygen::compat
