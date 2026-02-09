/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <moxygen/compat/Config.h>

#if MOXYGEN_USE_FOLLY
#include <folly/io/Cursor.h>
#include <folly/io/IOBufQueue.h>
#else
#include <moxygen/compat/ByteBufferQueue.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#endif

namespace moxygen::compat {

#if MOXYGEN_USE_FOLLY

// Folly mode: use native QueueAppender
using QueueAppender = folly::io::QueueAppender;

#else // !MOXYGEN_USE_FOLLY

// Std-mode replacement for folly::io::QueueAppender
// Writes data to a ByteBufferQueue in big-endian format
class QueueAppender {
 public:
  // Constructor matches folly::io::QueueAppender signature
  // queue: the buffer queue to write to
  // growth: suggested growth size for allocations (advisory only)
  explicit QueueAppender(ByteBufferQueue* queue, size_t growth = 256)
      : queue_(queue), growth_(growth) {}

  // Move constructor/assignment for use with lambdas
  QueueAppender(QueueAppender&& other) noexcept
      : queue_(other.queue_),
        growth_(other.growth_),
        buffer_(other.buffer_),
        bufferEnd_(other.bufferEnd_),
        cursor_(other.cursor_) {
    other.queue_ = nullptr;
    other.buffer_ = nullptr;
    other.bufferEnd_ = nullptr;
    other.cursor_ = nullptr;
  }

  QueueAppender& operator=(QueueAppender&& other) noexcept {
    if (this != &other) {
      flush();
      queue_ = other.queue_;
      growth_ = other.growth_;
      buffer_ = other.buffer_;
      bufferEnd_ = other.bufferEnd_;
      cursor_ = other.cursor_;
      other.queue_ = nullptr;
      other.buffer_ = nullptr;
      other.bufferEnd_ = nullptr;
      other.cursor_ = nullptr;
    }
    return *this;
  }

  // Non-copyable
  QueueAppender(const QueueAppender&) = delete;
  QueueAppender& operator=(const QueueAppender&) = delete;

  ~QueueAppender() {
    flush();
  }

  // Write a value in big-endian format
  template <typename T>
  void writeBE(T value) {
    static_assert(std::is_integral_v<T>, "writeBE requires integral type");
    ensureSpace(sizeof(T));

    // Write in big-endian order
    for (size_t i = 0; i < sizeof(T); ++i) {
      cursor_[i] = static_cast<uint8_t>(
          value >> (8 * (sizeof(T) - 1 - i)));
    }
    cursor_ += sizeof(T);
  }

  // Write a value in little-endian format
  template <typename T>
  void writeLE(T value) {
    static_assert(std::is_integral_v<T>, "writeLE requires integral type");
    ensureSpace(sizeof(T));

    // Write in little-endian order
    for (size_t i = 0; i < sizeof(T); ++i) {
      cursor_[i] = static_cast<uint8_t>(value >> (8 * i));
    }
    cursor_ += sizeof(T);
  }

  // Write raw bytes
  void push(const uint8_t* data, size_t len) {
    while (len > 0) {
      size_t available = bufferEnd_ - cursor_;
      if (available == 0) {
        ensureSpace(std::min(len, growth_));
        available = bufferEnd_ - cursor_;
      }

      size_t toCopy = std::min(len, available);
      std::memcpy(cursor_, data, toCopy);
      cursor_ += toCopy;
      data += toCopy;
      len -= toCopy;
    }
  }

  // Flush any pending data to the queue
  void flush() {
    if (queue_ && buffer_ && cursor_ > buffer_) {
      queue_->postallocate(cursor_ - buffer_);
      buffer_ = nullptr;
      bufferEnd_ = nullptr;
      cursor_ = nullptr;
    }
  }

 private:
  void ensureSpace(size_t needed) {
    size_t available = bufferEnd_ - cursor_;
    if (available >= needed) {
      return;
    }

    // Flush any current buffer
    flush();

    // Allocate new buffer
    size_t allocSize = std::max(needed, growth_);
    auto [ptr, size] = queue_->preallocate(needed, allocSize);
    buffer_ = static_cast<uint8_t*>(ptr);
    bufferEnd_ = buffer_ + size;
    cursor_ = buffer_;
  }

  ByteBufferQueue* queue_{nullptr};
  size_t growth_{256};
  uint8_t* buffer_{nullptr};
  uint8_t* bufferEnd_{nullptr};
  uint8_t* cursor_{nullptr};
};

#endif // MOXYGEN_USE_FOLLY

} // namespace moxygen::compat
