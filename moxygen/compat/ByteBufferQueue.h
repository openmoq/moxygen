/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <moxygen/compat/Config.h>

#if !MOXYGEN_USE_FOLLY

#include <moxygen/compat/ByteBuffer.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>

namespace moxygen::compat {

// Std-mode replacement for folly::IOBufQueue
// Manages a queue of ByteBuffers for incremental reading/writing
class ByteBufferQueue {
 public:
  struct Options {
    bool cacheChainLength{false};
  };

  ByteBufferQueue() = default;
  explicit ByteBufferQueue(Options opts) : cacheLength_(opts.cacheChainLength) {}

  // Convenience factory matching IOBufQueue::cacheChainLength()
  static Options cacheChainLength() {
    return Options{true};
  }

  // Move-only
  ByteBufferQueue(ByteBufferQueue&&) = default;
  ByteBufferQueue& operator=(ByteBufferQueue&&) = default;
  ByteBufferQueue(const ByteBufferQueue&) = delete;
  ByteBufferQueue& operator=(const ByteBufferQueue&) = delete;

  // Append data to the queue
  void append(std::unique_ptr<ByteBuffer> buf) {
    if (buf && buf->length() > 0) {
      if (cacheLength_) {
        cachedLength_ += buf->length();
      }
      chain_.push_back(std::move(buf));
    }
  }

  void append(const void* data, size_t len) {
    append(ByteBuffer::copyBuffer(data, len));
  }

  void append(const std::string& str) {
    append(str.data(), str.size());
  }

  // Prepend data to the queue
  void prepend(std::unique_ptr<ByteBuffer> buf) {
    if (buf && buf->length() > 0) {
      if (cacheLength_) {
        cachedLength_ += buf->length();
      }
      chain_.push_front(std::move(buf));
    }
  }

  // Get total length of all buffers
  size_t chainLength() const {
    if (cacheLength_) {
      return cachedLength_;
    }
    size_t total = 0;
    for (const auto& buf : chain_) {
      total += buf->length();
    }
    return total;
  }

  bool empty() const {
    return chain_.empty();
  }

  // Get the front buffer (for Cursor construction, matches IOBufQueue::front())
  const ByteBuffer* front() const {
    if (chain_.empty()) {
      return nullptr;
    }
    return chain_.front().get();
  }

  // Get pointer to front buffer's data
  const uint8_t* frontData() const {
    if (chain_.empty()) {
      return nullptr;
    }
    return chain_.front()->data();
  }

  // Move all data out as a single buffer
  std::unique_ptr<ByteBuffer> move() {
    if (chain_.empty()) {
      return nullptr;
    }

    // Coalesce into single buffer
    size_t totalLen = chainLength();
    auto result = ByteBuffer::create(totalLen);

    size_t offset = 0;
    for (auto& buf : chain_) {
      std::memcpy(result->writableData() + offset, buf->data(), buf->length());
      offset += buf->length();
    }
    result->append(totalLen);

    chain_.clear();
    if (cacheLength_) {
      cachedLength_ = 0;
    }

    return result;
  }

  // Split off first n bytes
  std::unique_ptr<ByteBuffer> split(size_t n) {
    if (n == 0 || chain_.empty()) {
      return nullptr;
    }

    size_t totalLen = chainLength();
    n = std::min(n, totalLen);

    auto result = ByteBuffer::create(n);
    size_t remaining = n;
    size_t offset = 0;

    while (remaining > 0 && !chain_.empty()) {
      auto& front = chain_.front();
      size_t toCopy = std::min(remaining, front->length());

      std::memcpy(result->writableData() + offset, front->data(), toCopy);
      offset += toCopy;
      remaining -= toCopy;

      if (toCopy == front->length()) {
        if (cacheLength_) {
          cachedLength_ -= front->length();
        }
        chain_.pop_front();
      } else {
        front->trimStart(toCopy);
        if (cacheLength_) {
          cachedLength_ -= toCopy;
        }
      }
    }

    result->append(n);
    return result;
  }

  // Trim bytes from the start
  void trimStart(size_t n) {
    while (n > 0 && !chain_.empty()) {
      auto& front = chain_.front();
      if (n >= front->length()) {
        n -= front->length();
        if (cacheLength_) {
          cachedLength_ -= front->length();
        }
        chain_.pop_front();
      } else {
        front->trimStart(n);
        if (cacheLength_) {
          cachedLength_ -= n;
        }
        n = 0;
      }
    }
  }

  // Clear all data
  void clear() {
    chain_.clear();
    if (cacheLength_) {
      cachedLength_ = 0;
    }
  }

  // Access to chain for iteration (used by Cursor)
  const std::deque<std::unique_ptr<ByteBuffer>>& chain() const {
    return chain_;
  }

  // Preallocate space in the buffer (returns {pointer, available_size})
  // This is used for writing frame headers where the size needs to be
  // filled in later after the body is written.
  std::pair<void*, size_t> preallocate(size_t min, size_t newAllocationSize) {
    // Create a new buffer with the requested size
    preallocatedBuf_ = ByteBuffer::create(newAllocationSize);
    preallocatedSize_ = 0;
    return std::make_pair(preallocatedBuf_->writableData(), newAllocationSize);
  }

  // Finalize the preallocated bytes
  void postallocate(size_t n) {
    if (preallocatedBuf_) {
      preallocatedBuf_->append(n);
      preallocatedSize_ = n;
      if (cacheLength_) {
        cachedLength_ += n;
      }
      chain_.push_back(std::move(preallocatedBuf_));
    }
  }

 private:
  std::unique_ptr<ByteBuffer> preallocatedBuf_;
  size_t preallocatedSize_{0};
  std::deque<std::unique_ptr<ByteBuffer>> chain_;
  bool cacheLength_{false};
  size_t cachedLength_{0};
};

} // namespace moxygen::compat

#endif // !MOXYGEN_USE_FOLLY
