/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <moxygen/compat/Config.h>

#if !MOXYGEN_USE_FOLLY

#include <moxygen/compat/ByteBuffer.h>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <stdexcept>
#include <vector>

// Debug assertions for ByteBufferQueue
#ifdef NDEBUG
#define QUEUE_DCHECK(cond) ((void)0)
#define QUEUE_DCHECK_EQ(a, b) ((void)0)
#define QUEUE_DCHECK_LE(a, b) ((void)0)
#define QUEUE_DCHECK_GE(a, b) ((void)0)
#else
#define QUEUE_DCHECK(cond) assert(cond)
#define QUEUE_DCHECK_EQ(a, b) assert((a) == (b))
#define QUEUE_DCHECK_LE(a, b) assert((a) <= (b))
#define QUEUE_DCHECK_GE(a, b) assert((a) >= (b))
#endif

#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/uio.h>
#endif

namespace moxygen::compat {

// Forward declaration
class ByteBufferQueue;

// IOVec - platform-independent scatter-gather I/O vector
// Matches struct iovec on POSIX, WSABUF on Windows
struct IOVec {
  void* data;
  size_t length;

#ifndef _WIN32
  // Convert to platform iovec for writev()/readv()
  struct iovec toIovec() const {
    struct iovec iov;
    iov.iov_base = data;
    iov.iov_len = length;
    return iov;
  }
#endif
};

// ChainView - provides iteration over buffer chain without coalescing
// Use this when the transport supports scatter-gather I/O
//
// SCATTER-GATHER I/O OVERVIEW:
// ============================
// Scatter-gather I/O (also called vectored I/O) allows reading/writing
// multiple non-contiguous buffers in a single system call, avoiding the
// need to copy data into a contiguous buffer first.
//
// POSIX APIs:
//   - writev(fd, iovec*, count) - write multiple buffers atomically
//   - readv(fd, iovec*, count)  - read into multiple buffers
//   - sendmsg()/recvmsg()       - with msghdr containing iovec array
//
// Benefits:
//   1. ZERO-COPY: No need to coalesce buffers before sending
//   2. FEWER SYSCALLS: One writev() vs multiple write() calls
//   3. ATOMICITY: Data written atomically (important for framing)
//   4. REDUCED LATENCY: No memcpy overhead
//
// Example usage with ChainView:
//
//   ByteBufferQueue queue;
//   queue.append(header);
//   queue.append(payload);
//
//   // BAD: Coalesces into single buffer (copies data)
//   auto buf = queue.move();
//   write(fd, buf->data(), buf->length());
//
//   // GOOD: Zero-copy scatter-gather
//   ChainView view = queue.moveAsChainView();
//   auto iovecs = view.getIovecs();
//   writev(fd, iovecs.data(), iovecs.size());
//
// When to use scatter-gather:
//   - Sending framed protocol messages (header + body)
//   - High-throughput scenarios where copy overhead matters
//   - When data naturally arrives in chunks (streaming)
//
// When NOT to use:
//   - Single small buffer (overhead of iovec setup)
//   - Transport doesn't support vectored I/O
//   - Need to inspect/modify combined data
//
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

  // Get IOVec array for scatter-gather I/O (writev/sendmsg)
  // Returns vector of {data, length} pairs ready for vectored I/O
  std::vector<IOVec> getIovecs() const {
    std::vector<IOVec> result;
    result.reserve(chain_.size());
    for (const auto& buf : chain_) {
      if (buf && buf->length() > 0) {
        result.push_back(
            IOVec{const_cast<uint8_t*>(buf->data()), buf->length()});
      }
    }
    return result;
  }

#ifndef _WIN32
  // Get platform-native iovec array for writev()/readv()
  std::vector<struct iovec> getIovecsNative() const {
    std::vector<struct iovec> result;
    result.reserve(chain_.size());
    for (const auto& buf : chain_) {
      if (buf && buf->length() > 0) {
        struct iovec iov;
        iov.iov_base = const_cast<uint8_t*>(buf->data());
        iov.iov_len = buf->length();
        result.push_back(iov);
      }
    }
    return result;
  }
#endif

  // Number of buffers (for iovec count parameter)
  size_t bufferCount() const {
    return chain_.size();
  }

 private:
  Container chain_;
  size_t totalLength_{0};
};

// Std-mode replacement for folly::IOBufQueue
// Manages a queue of ByteBuffers for incremental reading/writing
//
// Optimizations:
// - Single-buffer move() returns without copying (O(1))
// - trimStart() uses O(1) ByteBuffer::trimStart via offset adjustment
// - ChainView allows scatter-gather I/O without coalescing
// - chainLength() is always O(1) via cached length
// - Writable tail: small appends reuse existing buffer's tailroom
// - gather() returns IOVec array for writev()/sendmsg()
//
class ByteBufferQueue {
 public:
  struct Options {
    bool cacheChainLength{true};  // Always cache by default for O(1) length
  };

  ByteBufferQueue() = default;
  explicit ByteBufferQueue(Options /*opts*/) {
    // Length is always cached now - options kept for API compatibility
  }

  // Convenience factory matching IOBufQueue::cacheChainLength()
  static Options cacheChainLength() {
    return Options{true};
  }

  // Move-only
  ByteBufferQueue(ByteBufferQueue&&) = default;
  ByteBufferQueue& operator=(ByteBufferQueue&&) = default;
  ByteBufferQueue(const ByteBufferQueue&) = delete;
  ByteBufferQueue& operator=(const ByteBufferQueue&) = delete;

  // Append a buffer to the queue
  void append(std::unique_ptr<ByteBuffer> buf) {
    if (buf && buf->length() > 0) {
      cachedLength_ += buf->length();
      chain_.push_back(std::move(buf));
    }
  }

  // Append raw data - uses writable tail optimization when possible
  void append(const void* data, size_t len) {
    if (len == 0) {
      return;
    }

    // WRITABLE TAIL OPTIMIZATION:
    // If last buffer has enough tailroom, append there instead of
    // creating a new buffer. This reduces allocations for small appends.
    if (!chain_.empty()) {
      auto& tail = chain_.back();
      size_t tailroom = tail->tailroom();
      if (tailroom >= len) {
        // Append directly to existing buffer
        std::memcpy(tail->writableData() + tail->length(), data, len);
        tail->append(len);
        cachedLength_ += len;
        return;
      }
    }

    // No tailroom or empty queue - create new buffer
    append(ByteBuffer::copyBuffer(data, len));
  }

  void append(const std::string& str) {
    append(str.data(), str.size());
  }

  // Append another queue (merge) - O(1) by moving buffers
  void append(ByteBufferQueue&& other) {
    if (other.empty()) {
      return;
    }
    cachedLength_ += other.cachedLength_;
    for (auto& buf : other.chain_) {
      chain_.push_back(std::move(buf));
    }
    other.chain_.clear();
    other.cachedLength_ = 0;
  }

  // Prepend data to the queue
  void prepend(std::unique_ptr<ByteBuffer> buf) {
    if (buf && buf->length() > 0) {
      cachedLength_ += buf->length();
      chain_.push_front(std::move(buf));
    }
  }

  // Prepend another queue - O(n) where n is other's buffer count
  void prepend(ByteBufferQueue&& other) {
    if (other.empty()) {
      return;
    }
    cachedLength_ += other.cachedLength_;
    // Insert in reverse order to maintain correct sequence
    while (!other.chain_.empty()) {
      chain_.push_front(std::move(other.chain_.back()));
      other.chain_.pop_back();
    }
    other.cachedLength_ = 0;
  }

  // Wrap external data without copying (TRUE ZERO-COPY)
  //
  // WARNING - LIFETIME REQUIREMENTS:
  // The caller MUST ensure that:
  // 1. The external data remains valid until the buffer is consumed/destroyed
  // 2. The external data is not modified while this queue holds the reference
  // 3. This is ideal for:
  //    - Memory-mapped files
  //    - Static/const data
  //    - Data with guaranteed longer lifetime than the queue
  //
  // The wrapped buffer is READ-ONLY. Any attempt to modify will throw.
  // Use clone() if you need a writable copy.
  //
  void wrapBuffer(const void* data, size_t len) {
    if (len == 0) {
      return;
    }
    // TRUE ZERO-COPY: Use ByteBuffer's external data support
    auto buf = ByteBuffer::wrapExternal(data, len);
    cachedLength_ += len;
    chain_.push_back(std::move(buf));
  }

  // Wrap external data and immediately clone to owned buffer
  // Use this when you want the convenience of wrapBuffer but need writable data
  void wrapBufferCopy(const void* data, size_t len) {
    if (len == 0) {
      return;
    }
    append(ByteBuffer::copyBuffer(data, len));
  }

  // Get total length of all buffers - O(1) via cached length
  size_t chainLength() const {
    return cachedLength_;
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
      QUEUE_DCHECK_EQ(cachedLength_, size_t{0});
      return nullptr;
    }

    // FAST PATH: single buffer - just move it out (O(1))
    if (chain_.size() == 1) {
      auto result = std::move(chain_.front());
      QUEUE_DCHECK_EQ(cachedLength_, result->length());
      chain_.pop_front();
      cachedLength_ = 0;
      return result;
    }

    // Multiple buffers: coalesce
    return moveCoalesced();
  }

  // Move as ChainView - allows iteration without coalescing
  // Use this when transport supports scatter-gather I/O
  ChainView moveAsChainView() {
    ChainView view(std::move(chain_), cachedLength_);
    chain_.clear();
    cachedLength_ = 0;
    return view;
  }

  // Split off first n bytes as a single coalesced buffer
  // OPTIMIZED: Uses O(1) trimStart on remaining buffer
  std::unique_ptr<ByteBuffer> split(size_t n) {
    if (n == 0 || chain_.empty()) {
      return nullptr;
    }

    n = std::min(n, cachedLength_);

    // Fast path: split exactly at first buffer boundary
    auto& front = chain_.front();
    if (n == front->length()) {
      auto result = std::move(chain_.front());
      cachedLength_ -= result->length();
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
      cachedLength_ -= n;
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
        cachedLength_ -= buf->length();
        chain_.pop_front();
      } else {
        // O(1) trimStart with new ByteBuffer implementation
        buf->trimStart(toCopy);
        cachedLength_ -= toCopy;
      }
    }

    result->append(n);
    return result;
  }

  // Split off first n bytes as a chain of buffers (zero-copy when possible)
  // Returns a ChainView containing complete buffers, plus a partial copy if needed
  // This avoids coalescing when the split aligns with buffer boundaries
  ChainView splitChain(size_t n) {
    if (n == 0 || chain_.empty()) {
      return ChainView();
    }

    n = std::min(n, cachedLength_);
    std::deque<std::unique_ptr<ByteBuffer>> result;
    size_t resultLen = 0;

    // Move complete buffers that fit within n
    while (!chain_.empty() && resultLen + chain_.front()->length() <= n) {
      auto& front = chain_.front();
      resultLen += front->length();
      cachedLength_ -= front->length();
      result.push_back(std::move(chain_.front()));
      chain_.pop_front();
    }

    // Handle partial buffer if needed
    if (resultLen < n && !chain_.empty()) {
      size_t partialLen = n - resultLen;
      auto& front = chain_.front();

      // Create a copy for the partial portion
      auto partial = ByteBuffer::create(partialLen);
      std::memcpy(partial->writableData(), front->data(), partialLen);
      partial->append(partialLen);
      result.push_back(std::move(partial));

      // O(1) trimStart on remaining
      front->trimStart(partialLen);
      cachedLength_ -= partialLen;
      resultLen = n;
    }

    return ChainView(std::move(result), resultLen);
  }

  // Trim bytes from the start
  // OPTIMIZED: ByteBuffer::trimStart is now O(1)
  void trimStart(size_t n) {
    while (n > 0 && !chain_.empty()) {
      auto& front = chain_.front();
      if (n >= front->length()) {
        n -= front->length();
        cachedLength_ -= front->length();
        chain_.pop_front();
      } else {
        // O(1) trimStart with new ByteBuffer implementation
        front->trimStart(n);
        cachedLength_ -= n;
        n = 0;
      }
    }
  }

  // Trim bytes from the end
  // O(1) per buffer affected (ByteBuffer::trimEnd is O(1))
  void trimEnd(size_t n) {
    while (n > 0 && !chain_.empty()) {
      auto& back = chain_.back();
      if (n >= back->length()) {
        n -= back->length();
        cachedLength_ -= back->length();
        chain_.pop_back();
      } else {
        // O(1) trimEnd
        back->trimEnd(n);
        cachedLength_ -= n;
        n = 0;
      }
    }
    // Invariant check
    QUEUE_DCHECK(verifyCachedLength());
  }

  // Verify cached length matches actual (debug only)
  bool verifyCachedLength() const {
#ifdef NDEBUG
    return true;
#else
    size_t actual = 0;
    for (const auto& buf : chain_) {
      if (buf) {
        actual += buf->length();
      }
    }
    return actual == cachedLength_;
#endif
  }

  // Clear all data
  void clear() {
    chain_.clear();
    cachedLength_ = 0;
    preallocatedBuf_.reset();
    preallocatedCapacity_ = 0;
  }

  // Access to chain for iteration (used by Cursor)
  const std::deque<std::unique_ptr<ByteBuffer>>& chain() const {
    return chain_;
  }

  // Get IOVec array for scatter-gather I/O
  // Use with writev(), sendmsg(), or similar vectored I/O calls
  std::vector<IOVec> gather() const {
    std::vector<IOVec> result;
    result.reserve(chain_.size());
    for (const auto& buf : chain_) {
      if (buf && buf->length() > 0) {
        result.push_back(
            IOVec{const_cast<uint8_t*>(buf->data()), buf->length()});
      }
    }
    return result;
  }

#ifndef _WIN32
  // Get platform-native iovec array for writev()/readv()
  std::vector<struct iovec> gatherNative() const {
    std::vector<struct iovec> result;
    result.reserve(chain_.size());
    for (const auto& buf : chain_) {
      if (buf && buf->length() > 0) {
        struct iovec iov;
        iov.iov_base = const_cast<uint8_t*>(buf->data());
        iov.iov_len = buf->length();
        result.push_back(iov);
      }
    }
    return result;
  }
#endif

  // Clone the entire queue (deep copy)
  ByteBufferQueue clone() const {
    ByteBufferQueue copy;
    for (const auto& buf : chain_) {
      if (buf) {
        copy.append(buf->clone());
      }
    }
    return copy;
  }

  // Clone first buffer only
  std::unique_ptr<ByteBuffer> cloneOne() const {
    if (chain_.empty()) {
      return nullptr;
    }
    return chain_.front()->clone();
  }

  // Preallocate space in the buffer (returns {pointer, available_size})
  // This is used for writing frame headers where the size needs to be
  // filled in later after the body is written.
  //
  // Parameters:
  //   min - Minimum required size (will throw if newAllocationSize < min)
  //   newAllocationSize - Size to allocate
  //
  // Returns: {writable_pointer, actual_capacity}
  //
  // IMPORTANT: Must call postallocate() exactly once after preallocate()
  std::pair<void*, size_t> preallocate(size_t min, size_t newAllocationSize) {
    if (newAllocationSize < min) {
      throw std::invalid_argument(
          "ByteBufferQueue::preallocate: newAllocationSize < min");
    }

    if (preallocatedBuf_) {
      throw std::logic_error(
          "ByteBufferQueue::preallocate: previous preallocate not finalized");
    }

    preallocatedBuf_ = ByteBuffer::create(newAllocationSize);
    preallocatedCapacity_ = newAllocationSize;
    return std::make_pair(preallocatedBuf_->writableData(), newAllocationSize);
  }

  // Finalize the preallocated bytes
  // n must be <= the size returned by preallocate()
  void postallocate(size_t n) {
    if (!preallocatedBuf_) {
      throw std::logic_error(
          "ByteBufferQueue::postallocate: no pending preallocate");
    }

    if (n > preallocatedCapacity_) {
      throw std::invalid_argument(
          "ByteBufferQueue::postallocate: n exceeds preallocated capacity");
    }

    preallocatedBuf_->append(n);
    cachedLength_ += n;
    chain_.push_back(std::move(preallocatedBuf_));
    preallocatedCapacity_ = 0;
  }

  // Check if there's a pending preallocate
  bool hasPendingPreallocate() const {
    return preallocatedBuf_ != nullptr;
  }

  // Cancel a pending preallocate without adding to queue
  void cancelPreallocate() {
    preallocatedBuf_.reset();
    preallocatedCapacity_ = 0;
  }

  // Get the preallocated capacity (for partial use decisions)
  size_t preallocatedCapacity() const {
    return preallocatedCapacity_;
  }

  // Finalize partial preallocate and return unused portion for reuse
  // This is an optimization for when you preallocated more than needed
  // and want to reuse the remaining capacity for the next write.
  //
  // Returns: {pointer_to_remaining, remaining_capacity}
  //          or {nullptr, 0} if all capacity was used
  //
  std::pair<void*, size_t> postallocatePartial(size_t used) {
    if (!preallocatedBuf_) {
      throw std::logic_error(
          "ByteBufferQueue::postallocatePartial: no pending preallocate");
    }

    if (used > preallocatedCapacity_) {
      throw std::invalid_argument(
          "ByteBufferQueue::postallocatePartial: used exceeds capacity");
    }

    if (used == 0) {
      // Nothing used - just return the remaining capacity
      void* ptr = preallocatedBuf_->writableData();
      size_t cap = preallocatedCapacity_;
      // Don't add to chain, keep preallocated state
      return std::make_pair(ptr, cap);
    }

    // Commit the used portion
    preallocatedBuf_->append(used);
    cachedLength_ += used;

    size_t remaining = preallocatedCapacity_ - used;

    if (remaining > 0 && preallocatedBuf_->tailroom() >= remaining) {
      // There's useful tailroom left - the next append can use it
      chain_.push_back(std::move(preallocatedBuf_));
      preallocatedCapacity_ = 0;
      // Return pointer to tailroom for potential direct writes
      auto& back = chain_.back();
      return std::make_pair(
          back->writableData() + back->length(),
          back->tailroom());
    }

    // No useful remaining capacity
    chain_.push_back(std::move(preallocatedBuf_));
    preallocatedCapacity_ = 0;
    return std::make_pair(nullptr, size_t{0});
  }

 private:
  // Coalesce all buffers into one (called when chain_.size() > 1)
  std::unique_ptr<ByteBuffer> moveCoalesced() {
    auto result = ByteBuffer::create(cachedLength_);

    size_t offset = 0;
    for (auto& buf : chain_) {
      std::memcpy(result->writableData() + offset, buf->data(), buf->length());
      offset += buf->length();
    }
    result->append(cachedLength_);

    chain_.clear();
    cachedLength_ = 0;

    return result;
  }

  std::unique_ptr<ByteBuffer> preallocatedBuf_;
  size_t preallocatedCapacity_{0};  // Capacity of preallocated buffer
  std::deque<std::unique_ptr<ByteBuffer>> chain_;
  size_t cachedLength_{0};  // Always cached for O(1) chainLength()
};

} // namespace moxygen::compat

#endif // !MOXYGEN_USE_FOLLY
