/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <moxygen/compat/Config.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

// Debug assertions - active in debug builds only
#ifdef NDEBUG
#define BYTEBUFFER_DCHECK(cond) ((void)0)
#define BYTEBUFFER_DCHECK_LE(a, b) ((void)0)
#define BYTEBUFFER_DCHECK_GE(a, b) ((void)0)
#else
#define BYTEBUFFER_DCHECK(cond) assert(cond)
#define BYTEBUFFER_DCHECK_LE(a, b) assert((a) <= (b))
#define BYTEBUFFER_DCHECK_GE(a, b) assert((a) >= (b))
#endif

namespace moxygen::compat {

#if MOXYGEN_USE_FOLLY

// In Folly mode, ByteBuffer is not used directly - use IOBuf via Payload
// This header provides the std-mode implementation only

#else // !MOXYGEN_USE_FOLLY

// Thread-local buffer pool for reducing allocation overhead
// Pools heap buffers by size class (powers of 2 from 256 to 64KB)
class ByteBufferPool {
 public:
  // Size classes: 256, 512, 1K, 2K, 4K, 8K, 16K, 32K, 64K
  static constexpr size_t kMinPoolSize = 256;
  static constexpr size_t kMaxPoolSize = 65536;
  static constexpr size_t kNumSizeClasses = 9;  // log2(64K/256) + 1
  static constexpr size_t kMaxPooledPerClass = 8;  // Max buffers to keep per size

  // Get thread-local pool instance
  static ByteBufferPool& instance() {
    thread_local ByteBufferPool pool;
    return pool;
  }

  // Allocate a buffer of at least the requested size
  // Returns nullptr if no pooled buffer available (caller should use new[])
  uint8_t* allocate(size_t requestedSize, size_t& actualSize) {
    if (requestedSize < kMinPoolSize || requestedSize > kMaxPoolSize) {
      return nullptr;
    }

    size_t sizeClass = getSizeClass(requestedSize);
    actualSize = classSizes_[sizeClass];

    auto& pool = pools_[sizeClass];
    if (!pool.empty()) {
      uint8_t* buf = pool.back();
      pool.pop_back();
      return buf;
    }
    return nullptr;
  }

  // Return a buffer to the pool
  // Returns true if pooled, false if caller should delete[]
  bool deallocate(uint8_t* buf, size_t size) {
    if (size < kMinPoolSize || size > kMaxPoolSize || !buf) {
      return false;
    }

    size_t sizeClass = getSizeClass(size);

    auto& pool = pools_[sizeClass];
    if (pool.size() < kMaxPooledPerClass) {
      pool.push_back(buf);
      return true;
    }
    return false;  // Pool full, caller should delete
  }

  ~ByteBufferPool() {
    // Free all pooled buffers
    for (auto& pool : pools_) {
      for (auto* buf : pool) {
        delete[] buf;
      }
    }
  }

 private:
  ByteBufferPool() {
    // Initialize size class lookup
    size_t size = kMinPoolSize;
    for (size_t i = 0; i < kNumSizeClasses; ++i) {
      classSizes_[i] = size;
      size *= 2;
    }
  }

  // Get size class index for a given size (rounds up to next power of 2)
  size_t getSizeClass(size_t size) const {
    // Find the smallest size class that fits
    for (size_t i = 0; i < kNumSizeClasses; ++i) {
      if (classSizes_[i] >= size) {
        return i;
      }
    }
    return kNumSizeClasses - 1;  // Largest class
  }

  std::array<size_t, kNumSizeClasses> classSizes_;
  std::array<std::vector<uint8_t*>, kNumSizeClasses> pools_;
};

// Std-mode ByteBuffer - IOBuf-like buffer with headroom/tailroom and SBO
//
// Features:
// - O(1) trimStart() via offset adjustment (like IOBuf headroom)
// - O(1) prepend() when headroom available
// - Small Buffer Optimization: buffers <= 64 bytes use inline storage
// - External data wrapping for zero-copy operations
// - Overflow-safe arithmetic in append/prepend
// - Thread-local buffer pooling for reduced allocations
//
// Memory layout (heap mode):
// |<-- headroom -->|<------- length -------->|<--- tailroom --->|
// |   (offset_)    |      (valid data)       |                  |
// +----------------+-------------------------+------------------+
// ^                ^                         ^                  ^
// heap_.data    data()              data()+length()    heap_.data+capacity
//
// Ownership modes:
// - OWNED: Buffer owns the memory (inline, heap, or pooled)
// - EXTERNAL: Buffer references external memory (caller owns lifetime)
//
class ByteBuffer {
 public:
  // Configuration constants
  static constexpr size_t kInlineSize = 64;        // SBO threshold
  static constexpr size_t kDefaultHeadroom = 128;  // Pre-allocated headroom
  static constexpr size_t kDefaultTailroom = 256;  // Pre-allocated tailroom

  // Ownership mode
  enum class OwnershipMode : uint8_t {
    OWNED,     // Buffer owns the memory
    EXTERNAL   // External memory, caller manages lifetime
  };

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
      size_t requestedSize = capacity + kDefaultHeadroom + kDefaultTailroom;

      // Try to get from pool first
      size_t actualSize = 0;
      uint8_t* pooled = ByteBufferPool::instance().allocate(requestedSize, actualSize);
      if (pooled) {
        storage_.heap_.data = pooled;
        storage_.heap_.capacity = actualSize;
        fromPool_ = true;
      } else {
        storage_.heap_.data = new uint8_t[requestedSize];
        storage_.heap_.capacity = requestedSize;
        fromPool_ = false;
      }
      offset_ = kDefaultHeadroom;  // Start with headroom
    }
  }

  // Private constructor for external data wrapping
 private:
  ByteBuffer(const uint8_t* externalData, size_t len, OwnershipMode mode)
      : isInline_(false),
        isExternal_(mode == OwnershipMode::EXTERNAL),
        offset_(0),
        length_(len) {
    // Store external pointer in heap storage (won't be freed)
    storage_.heap_.data = const_cast<uint8_t*>(externalData);
    storage_.heap_.capacity = len;  // No headroom/tailroom for external
  }

 public:
  // Destructor - return to pool, delete, or nothing (external)
  ~ByteBuffer() {
    freeOwnedMemory();
  }

  // Move constructor
  ByteBuffer(ByteBuffer&& other) noexcept
      : isInline_(other.isInline_),
        fromPool_(other.fromPool_),
        isExternal_(other.isExternal_),
        offset_(other.offset_),
        length_(other.length_) {
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
    other.fromPool_ = false;
    other.isExternal_ = false;
  }

  // Move assignment
  ByteBuffer& operator=(ByteBuffer&& other) noexcept {
    if (this != &other) {
      // Clean up current memory
      freeOwnedMemory();

      isInline_ = other.isInline_;
      fromPool_ = other.fromPool_;
      isExternal_ = other.isExternal_;
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
      other.fromPool_ = false;
      other.isExternal_ = false;
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

  // Wrap external data without copying (ZERO-COPY)
  //
  // WARNING: The caller MUST ensure that:
  // 1. The external data remains valid for the lifetime of this ByteBuffer
  // 2. The external data is not modified while this ByteBuffer exists
  // 3. This ByteBuffer is not used after the external data is freed
  //
  // Use cases:
  // - Wrapping data from memory-mapped files
  // - Wrapping data from static/const buffers
  // - Avoiding copies when data lifetime is guaranteed
  //
  // The wrapped buffer is READ-ONLY. Calls to writableData() will throw.
  // Operations that modify the buffer (prepend, append to data) will throw.
  // trimStart/trimEnd are allowed as they only adjust offsets.
  //
  static std::unique_ptr<ByteBuffer> wrapExternal(
      const void* data,
      size_t size) {
    if (!data && size > 0) {
      throw std::invalid_argument("ByteBuffer::wrapExternal: null data with non-zero size");
    }
    // Use placement new with private constructor
    auto buf = std::unique_ptr<ByteBuffer>(new ByteBuffer(
        static_cast<const uint8_t*>(data), size, OwnershipMode::EXTERNAL));
    return buf;
  }

  // Check if buffer wraps external data
  bool isExternal() const {
    return isExternal_;
  }

  // Check if buffer is writable (not external)
  bool isWritable() const {
    return !isExternal_;
  }

  // Data access - accounts for offset
  // THROWS if buffer is external (read-only)
  uint8_t* writableData() {
    if (isExternal_) {
      throw std::logic_error(
          "ByteBuffer::writableData: cannot write to external buffer");
    }
    if (isInline_) {
      return storage_.inline_.data() + offset_;
    }
    return storage_.heap_.data + offset_;
  }

  // Safe version that returns nullptr for external buffers
  uint8_t* writableDataOrNull() {
    if (isExternal_) {
      return nullptr;
    }
    if (isInline_) {
      return storage_.inline_.data() + offset_;
    }
    return storage_.heap_.data + offset_;
  }

  const uint8_t* data() const {
    if (isInline_) {
      BYTEBUFFER_DCHECK_LE(offset_, kInlineSize);
      return storage_.inline_.data() + offset_;
    }
    BYTEBUFFER_DCHECK(storage_.heap_.data != nullptr || length_ == 0);
    BYTEBUFFER_DCHECK_LE(offset_, storage_.heap_.capacity);
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
  // For external buffers: only extends length, doesn't allow writing past original
  void append(size_t len) {
    if (isExternal_) {
      // For external buffers, we can only "reveal" more of the original data
      // This is used after wrapExternal to set the initial length
      size_t maxLen = storage_.heap_.capacity;  // Original external size
      if (length_ + len > maxLen) {
        throw std::out_of_range(
            "ByteBuffer::append: would exceed external buffer bounds");
      }
      length_ += len;
      return;
    }

    size_t newLength;
    if (__builtin_add_overflow(length_, len, &newLength)) {
      throw std::overflow_error("ByteBuffer::append overflow");
    }
    // Cap at available tailroom
    length_ = std::min(newLength, length_ + tailroom());
  }

  // Prepend - O(1) when headroom available, else reallocate
  // THROWS for external buffers (cannot reallocate)
  void prepend(size_t len) {
    if (len == 0) return;

    if (isExternal_) {
      // External buffers have no headroom
      throw std::logic_error(
          "ByteBuffer::prepend: cannot prepend to external buffer");
    }

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
    // Invariant: offset_ + length_ never exceeds capacity
    BYTEBUFFER_DCHECK_LE(offset_ + length_, isInline_ ? kInlineSize : storage_.heap_.capacity);
  }

  // TrimEnd - O(1), just reduce length
  void trimEnd(size_t len) {
    if (len >= length_) {
      length_ = 0;
    } else {
      length_ -= len;
    }
    // Invariant: offset_ + length_ never exceeds capacity
    BYTEBUFFER_DCHECK_LE(offset_ + length_, isInline_ ? kInlineSize : storage_.heap_.capacity);
  }

  // Clone - deep copy (always creates owned buffer)
  // For external buffers, this is the way to get a writable copy
  std::unique_ptr<ByteBuffer> clone() const {
    auto copy = std::make_unique<ByteBuffer>(length_);
    std::memcpy(copy->writableData(), data(), length_);
    copy->length_ = length_;
    return copy;
  }

  // Clone into owned buffer only if external, otherwise return nullptr
  // Useful when you need a writable buffer but want to avoid copy if already owned
  std::unique_ptr<ByteBuffer> cloneIfExternal() const {
    if (isExternal_) {
      return clone();
    }
    return nullptr;
  }

  // Coalesce - no-op for contiguous buffer
  void coalesce() {}

  // Reserve more capacity (may reallocate)
  // THROWS for external buffers
  void reserve(size_t newCapacity) {
    if (isExternal_) {
      throw std::logic_error(
          "ByteBuffer::reserve: cannot reserve on external buffer");
    }
    if (newCapacity > capacity()) {
      reallocateForCapacity(newCapacity);
    }
  }

  // Ensure writable space at end
  // THROWS for external buffers
  void ensureWritableSpace(size_t len) {
    if (isExternal_) {
      throw std::logic_error(
          "ByteBuffer::ensureWritableSpace: cannot ensure space on external buffer");
    }
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
  bool fromPool_{false};    // Whether heap buffer came from pool
  bool isExternal_{false};  // Whether buffer wraps external data (not owned)
  size_t offset_{0};        // Headroom: valid data starts at offset_
  size_t length_{0};        // Valid data length

  // Free owned memory (skip for external buffers)
  void freeOwnedMemory() {
    if (isExternal_) {
      // External data - we don't own it, don't free
      return;
    }
    if (!isInline_ && storage_.heap_.data) {
      if (fromPool_) {
        if (!ByteBufferPool::instance().deallocate(
                storage_.heap_.data, storage_.heap_.capacity)) {
          delete[] storage_.heap_.data;
        }
      } else {
        delete[] storage_.heap_.data;
      }
    }
  }

  // Legacy alias for compatibility
  void freeHeapBuffer() {
    freeOwnedMemory();
  }

  // Allocate a new heap buffer (from pool or new)
  void allocateHeapBuffer(size_t size, size_t& actualCapacity) {
    size_t actualSize = 0;
    uint8_t* pooled = ByteBufferPool::instance().allocate(size, actualSize);
    if (pooled) {
      storage_.heap_.data = pooled;
      storage_.heap_.capacity = actualSize;
      actualCapacity = actualSize;
      fromPool_ = true;
    } else {
      storage_.heap_.data = new uint8_t[size];
      storage_.heap_.capacity = size;
      actualCapacity = size;
      fromPool_ = false;
    }
    isInline_ = false;
  }

  // Reallocate with additional headroom for prepend
  void reallocateWithHeadroom(size_t additionalHeadroom) {
    size_t neededHeadroom = additionalHeadroom + kDefaultHeadroom;
    size_t newCapacity = neededHeadroom + length_ + kDefaultTailroom;

    // Allocate new buffer (try pool first)
    size_t actualCapacity = 0;
    uint8_t* oldData = isInline_ ? storage_.inline_.data() : storage_.heap_.data;
    size_t oldLength = length_;
    size_t oldOffset = offset_;
    bool wasInline = isInline_;
    bool wasFromPool = fromPool_;

    // Save old heap info before allocating new
    uint8_t* oldHeapData = wasInline ? nullptr : storage_.heap_.data;
    size_t oldHeapCapacity = wasInline ? 0 : storage_.heap_.capacity;

    allocateHeapBuffer(newCapacity, actualCapacity);
    size_t newOffset = neededHeadroom;

    // Copy existing data to new location
    std::memcpy(storage_.heap_.data + newOffset, oldData + oldOffset, oldLength);

    // Clean up old heap if needed
    if (!wasInline && oldHeapData) {
      if (wasFromPool) {
        if (!ByteBufferPool::instance().deallocate(oldHeapData, oldHeapCapacity)) {
          delete[] oldHeapData;
        }
      } else {
        delete[] oldHeapData;
      }
    }

    offset_ = newOffset;
  }

  // Reallocate for more capacity (tailroom)
  void reallocateForCapacity(size_t newCapacity) {
    size_t allocSize = offset_ + newCapacity + kDefaultTailroom;

    // Save old state
    uint8_t* oldData = isInline_ ? storage_.inline_.data() : storage_.heap_.data;
    size_t oldLength = length_;
    size_t oldOffset = offset_;
    bool wasInline = isInline_;
    bool wasFromPool = fromPool_;
    uint8_t* oldHeapData = wasInline ? nullptr : storage_.heap_.data;
    size_t oldHeapCapacity = wasInline ? 0 : storage_.heap_.capacity;

    size_t actualCapacity = 0;
    allocateHeapBuffer(allocSize, actualCapacity);

    // Copy existing data preserving offset
    if (oldLength > 0) {
      std::memcpy(storage_.heap_.data + oldOffset, oldData + oldOffset, oldLength);
    }
    offset_ = oldOffset;

    // Clean up old heap if needed
    if (!wasInline && oldHeapData) {
      if (wasFromPool) {
        if (!ByteBufferPool::instance().deallocate(oldHeapData, oldHeapCapacity)) {
          delete[] oldHeapData;
        }
      } else {
        delete[] oldHeapData;
      }
    }
  }
};

#endif // MOXYGEN_USE_FOLLY

} // namespace moxygen::compat
