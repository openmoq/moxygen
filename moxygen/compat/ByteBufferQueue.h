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
#include <cstring>
#include <deque>
#include <memory>

namespace moxygen::compat {

// Forward declaration
class ByteBufferQueue;

// ChainView - provides iteration over buffer chain without coalescing
// Use this when the transport supports scatter-gather I/O
class ChainView {
 public:
  using BufferPtr = std::unique_ptr<ByteBuffer>;
  using Container = std::deque<BufferPtr>;

  ChainView() = default;

  ChainView(Container&& chain, size_t totalLength)
      : chain_(std::move(chain)), totalLength_(totalLength) {}

  // Move-only
  ChainView(ChainView&&) = default;
  ChainView& operator=(ChainView&&) = default;
  ChainView(const ChainView&) = delete;
  ChainView& operator=(const ChainView&) = delete;

  // Iteration over buffers
  auto begin() { return chain_.begin(); }
  auto end() { return chain_.end(); }
  auto begin() const { return chain_.cbegin(); }
  auto end() const { return chain_.cend(); }

  // Properties
  size_t chainLength() const { return totalLength_; }
  bool empty() const { return chain_.empty(); }
  size_t size() const { return chain_.size(); }

  // Access individual buffers
  ByteBuffer* front() {
    return chain_.empty() ? nullptr : chain_.front().get();
  }
  const ByteBuffer* front() const {
    return chain_.empty() ? nullptr : chain_.front().get();
  }

  // Lazy coalesce - only copies when actually needed
  std::unique_ptr<ByteBuffer> coalesce() {
    if (chain_.empty()) {
      return nullptr;
    }

    // Single buffer - just move it out
    if (chain_.size() == 1) {
      auto result = std::move(chain_.front());
      chain_.clear();
      totalLength_ = 0;
      return result;
    }

    // Multiple buffers - need to copy
    auto result = ByteBuffer::create(totalLength_);
    size_t offset = 0;
    for (auto& buf : chain_) {
      std::memcpy(result->writableData() + offset, buf->data(), buf->length());
      offset += buf->length();
    }
    result->append(totalLength_);
    chain_.clear();
    totalLength_ = 0;
    return result;
  }

  // Release ownership of a single buffer (only valid if size() == 1)
  std::unique_ptr<ByteBuffer> releaseSingle() {
    if (chain_.size() != 1) {
      return nullptr;
    }
    auto result = std::move(chain_.front());
    chain_.clear();
    totalLength_ = 0;
    return result;
  }

 private:
  Container chain_;
  size_t totalLength_{0};
};

// Std-mode replacement for folly::IOBufQueue
// Manages a queue of ByteBuffers for incremental reading/writing
//
// Optimizations over naive implementation:
// - Single-buffer move() returns without copying (O(1))
// - trimStart() uses O(1) ByteBuffer::trimStart via offset adjustment
// - ChainView allows scatter-gather I/O without coalescing
//
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
  // OPTIMIZED: Single-buffer case is O(1), no copy needed
  std::unique_ptr<ByteBuffer> move() {
    if (chain_.empty()) {
      return nullptr;
    }

    // FAST PATH: single buffer - just move it out (O(1))
    if (chain_.size() == 1) {
      auto result = std::move(chain_.front());
      chain_.pop_front();
      if (cacheLength_) {
        cachedLength_ = 0;
      }
      return result;
    }

    // Multiple buffers: coalesce
    return moveCoalesced();
  }

  // Move as ChainView - allows iteration without coalescing
  // Use this when transport supports scatter-gather I/O
  ChainView moveAsChainView() {
    size_t len = cacheLength_ ? cachedLength_ : chainLength();
    ChainView view(std::move(chain_), len);
    chain_.clear();
    if (cacheLength_) {
      cachedLength_ = 0;
    }
    return view;
  }

  // Split off first n bytes
  // OPTIMIZED: Uses O(1) trimStart on remaining buffer
  std::unique_ptr<ByteBuffer> split(size_t n) {
    if (n == 0 || chain_.empty()) {
      return nullptr;
    }

    size_t totalLen = chainLength();
    n = std::min(n, totalLen);

    // Fast path: split exactly at first buffer boundary
    auto& front = chain_.front();
    if (n == front->length()) {
      auto result = std::move(chain_.front());
      if (cacheLength_) {
        cachedLength_ -= result->length();
      }
      chain_.pop_front();
      return result;
    }

    // Split within first buffer
    if (n < front->length()) {
      auto result = ByteBuffer::create(n);
      std::memcpy(result->writableData(), front->data(), n);
      result->append(n);

      // O(1) trimStart with new ByteBuffer implementation
      front->trimStart(n);

      if (cacheLength_) {
        cachedLength_ -= n;
      }
      return result;
    }

    // Split spans multiple buffers - need to accumulate
    auto result = ByteBuffer::create(n);
    size_t remaining = n;
    size_t offset = 0;

    while (remaining > 0 && !chain_.empty()) {
      auto& buf = chain_.front();
      size_t toCopy = std::min(remaining, buf->length());

      std::memcpy(result->writableData() + offset, buf->data(), toCopy);
      offset += toCopy;
      remaining -= toCopy;

      if (toCopy == buf->length()) {
        if (cacheLength_) {
          cachedLength_ -= buf->length();
        }
        chain_.pop_front();
      } else {
        // O(1) trimStart with new ByteBuffer implementation
        buf->trimStart(toCopy);
        if (cacheLength_) {
          cachedLength_ -= toCopy;
        }
      }
    }

    result->append(n);
    return result;
  }

  // Trim bytes from the start
  // OPTIMIZED: ByteBuffer::trimStart is now O(1)
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
        // O(1) trimStart with new ByteBuffer implementation
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
  // Coalesce all buffers into one (called when chain_.size() > 1)
  std::unique_ptr<ByteBuffer> moveCoalesced() {
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

  std::unique_ptr<ByteBuffer> preallocatedBuf_;
  size_t preallocatedSize_{0};
  std::deque<std::unique_ptr<ByteBuffer>> chain_;
  bool cacheLength_{false};
  size_t cachedLength_{0};
};

} // namespace moxygen::compat

#endif // !MOXYGEN_USE_FOLLY
