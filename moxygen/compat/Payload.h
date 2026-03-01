/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <moxygen/compat/Config.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#if MOXYGEN_USE_FOLLY
#include <folly/io/IOBuf.h>
#else
#include <moxygen/compat/ByteBuffer.h>
#endif

namespace moxygen::compat {

#if MOXYGEN_USE_FOLLY

// Thin wrapper around folly::IOBuf providing IOBuf-compatible API
// This allows code to use Payload without directly depending on IOBuf
class Payload {
 public:
  Payload() = default;
  explicit Payload(std::unique_ptr<folly::IOBuf> buf) : buf_(std::move(buf)) {}

  // Move-only
  Payload(Payload&&) = default;
  Payload& operator=(Payload&&) = default;
  Payload(const Payload&) = delete;
  Payload& operator=(const Payload&) = delete;

  // Check if payload is valid/non-null
  explicit operator bool() const { return buf_ != nullptr; }

  // IOBuf-compatible API
  size_t computeChainDataLength() const {
    return buf_ ? buf_->computeChainDataLength() : 0;
  }

  size_t length() const { return buf_ ? buf_->length() : 0; }

  size_t size() const { return length(); }  // Alias for length()

  const uint8_t* data() const { return buf_ ? buf_->data() : nullptr; }

  uint8_t* writableData() { return buf_ ? buf_->writableData() : nullptr; }

  bool empty() const { return !buf_ || buf_->empty(); }

  void coalesce() {
    if (buf_) {
      buf_->coalesce();
    }
  }

  std::unique_ptr<Payload> clone() const {
    if (!buf_) {
      return nullptr;
    }
    return std::make_unique<Payload>(buf_->clone());
  }

  // String conversion (like moveToFbString but returns std::string)
  std::string moveToString() {
    if (!buf_) {
      return {};
    }
    buf_->coalesce();
    return std::string(
        reinterpret_cast<const char*>(buf_->data()), buf_->length());
  }

  // Non-mutating string conversion
  std::string toString() const {
    if (!buf_) {
      return {};
    }
    // Clone and coalesce to avoid modifying original
    auto clone = buf_->clone();
    clone->coalesce();
    return std::string(
        reinterpret_cast<const char*>(clone->data()), clone->length());
  }

  // Access underlying IOBuf for code that needs it (Cursor, IOBufQueue, etc.)
  folly::IOBuf* getIOBuf() { return buf_.get(); }
  const folly::IOBuf* getIOBuf() const { return buf_.get(); }

  // Release ownership of underlying IOBuf
  std::unique_ptr<folly::IOBuf> releaseIOBuf() { return std::move(buf_); }

  // Chain operations
  void appendChain(std::unique_ptr<Payload> other) {
    if (buf_ && other && other->buf_) {
      buf_->appendChain(std::move(other->buf_));
    }
  }

  void prependChain(std::unique_ptr<Payload> other) {
    if (other && other->buf_) {
      if (buf_) {
        other->buf_->appendChain(std::move(buf_));
      }
      buf_ = std::move(other->buf_);
    }
  }

  // For IOBuf-like append (extends writable tail)
  void append(size_t n) {
    if (buf_) {
      buf_->append(n);
    }
  }

  // Factory methods matching IOBuf API
  static std::unique_ptr<Payload> create(size_t capacity) {
    return std::make_unique<Payload>(folly::IOBuf::create(capacity));
  }

  static std::unique_ptr<Payload> copyBuffer(const void* data, size_t size) {
    return std::make_unique<Payload>(folly::IOBuf::copyBuffer(data, size));
  }

  static std::unique_ptr<Payload> copyBuffer(const std::string& str) {
    return std::make_unique<Payload>(
        folly::IOBuf::copyBuffer(str.data(), str.size()));
  }

  // Wrap existing IOBuf
  static std::unique_ptr<Payload> wrap(std::unique_ptr<folly::IOBuf> buf) {
    return std::make_unique<Payload>(std::move(buf));
  }

 private:
  std::unique_ptr<folly::IOBuf> buf_;
};

#else // !MOXYGEN_USE_FOLLY

// =============================================================================
// Std-mode Payload backed by ByteBuffer with reference counting
// =============================================================================

class Payload {
 public:
  Payload() = default;

  // Construct from vector (legacy compatibility)
  explicit Payload(std::vector<uint8_t> data) {
    if (!data.empty()) {
      buf_ = std::make_shared<ByteBuffer>(data.size());
      std::memcpy(buf_->writableData(), data.data(), data.size());
      buf_->append(data.size());
    }
  }

  // Construct from ByteBuffer (takes ownership)
  explicit Payload(std::unique_ptr<ByteBuffer> buf)
      : buf_(std::move(buf)) {}

  // Construct from shared ByteBuffer (reference counting)
  explicit Payload(std::shared_ptr<ByteBuffer> buf)
      : buf_(std::move(buf)) {}

  // Iterator constructor for convenience
  template <typename InputIt>
  Payload(InputIt first, InputIt last) {
    size_t size = std::distance(first, last);
    if (size > 0) {
      buf_ = std::make_shared<ByteBuffer>(size);
      std::copy(first, last, buf_->writableData());
      buf_->append(size);
    }
  }

  // Move operations
  Payload(Payload&&) = default;
  Payload& operator=(Payload&&) = default;

  // Copy is allowed (shares underlying buffer via refcount)
  Payload(const Payload& other) : buf_(other.buf_), next_(nullptr) {
    // Note: doesn't copy chain, only this buffer
  }
  Payload& operator=(const Payload& other) {
    if (this != &other) {
      buf_ = other.buf_;
      next_ = nullptr;
    }
    return *this;
  }

  explicit operator bool() const { return buf_ != nullptr && buf_->length() > 0; }

  // Total length of this buffer and all chained buffers
  size_t computeChainDataLength() const {
    size_t total = length();
    for (Payload* p = next_.get(); p != nullptr; p = p->next_.get()) {
      total += p->length();
    }
    return total;
  }

  size_t length() const { return buf_ ? buf_->length() : 0; }
  size_t size() const { return length(); }

  const uint8_t* data() const { return buf_ ? buf_->data() : nullptr; }

  // Get writable data - triggers copy-on-write if shared
  uint8_t* writableData() {
    ensureUnique();
    return buf_ ? buf_->writableData() : nullptr;
  }

  bool empty() const { return !buf_ || buf_->empty(); }

  // Coalesce chain into single contiguous buffer
  void coalesce() {
    if (!next_) {
      return;  // Already single buffer
    }

    size_t totalLen = computeChainDataLength();
    auto newBuf = std::make_shared<ByteBuffer>(totalLen);

    // Copy all data
    size_t offset = 0;
    for (Payload* p = this; p != nullptr; p = p->next_.get()) {
      if (p->buf_ && p->buf_->length() > 0) {
        std::memcpy(newBuf->writableData() + offset, p->buf_->data(), p->buf_->length());
        offset += p->buf_->length();
      }
    }
    newBuf->append(totalLen);

    buf_ = std::move(newBuf);
    next_.reset();
  }

  // Clone - shares buffer via reference counting (O(1))
  // Actual copy only happens on write (copy-on-write)
  std::unique_ptr<Payload> clone() const {
    if (!buf_) {
      return nullptr;
    }
    auto p = std::make_unique<Payload>(buf_);  // Share the buffer
    // Clone chain if present
    if (next_) {
      p->next_ = next_->clone();
    }
    return p;
  }

  std::string moveToString() {
    coalesce();
    if (!buf_) {
      return {};
    }
    return std::string(reinterpret_cast<const char*>(buf_->data()), buf_->length());
  }

  std::string toString() const {
    if (!buf_) {
      return {};
    }
    if (!next_) {
      return std::string(reinterpret_cast<const char*>(buf_->data()), buf_->length());
    }
    // Need to coalesce for chain
    std::string result;
    result.reserve(computeChainDataLength());
    for (const Payload* p = this; p != nullptr; p = p->next_.get()) {
      if (p->buf_) {
        result.append(reinterpret_cast<const char*>(p->buf_->data()), p->buf_->length());
      }
    }
    return result;
  }

  // No IOBuf in std mode
  void* getIOBuf() { return nullptr; }
  const void* getIOBuf() const { return nullptr; }

  // Append another payload to the chain
  void appendChain(std::unique_ptr<Payload> other) {
    if (!other) return;
    Payload* tail = this;
    while (tail->next_) {
      tail = tail->next_.get();
    }
    tail->next_ = std::move(other);
  }

  // Prepend another payload to the chain
  void prependChain(std::unique_ptr<Payload> other) {
    if (!other) return;
    // Find tail of other chain
    Payload* otherTail = other.get();
    while (otherTail->next_) {
      otherTail = otherTail->next_.get();
    }
    // Move our data to become next of other's tail
    otherTail->next_ = std::make_unique<Payload>(std::move(buf_));
    otherTail->next_->next_ = std::move(next_);
    // Take other's buffer as ours
    buf_ = std::move(other->buf_);
    next_ = std::move(other->next_);
  }

  // Extend valid data region
  void append(size_t n) {
    ensureUnique();
    if (buf_) {
      buf_->append(n);
    }
  }

  // Trim bytes from start (O(1) with ByteBuffer's offset)
  void trimStart(size_t n) {
    ensureUnique();
    if (buf_) {
      buf_->trimStart(n);
    }
  }

  // Trim bytes from end
  void trimEnd(size_t n) {
    ensureUnique();
    if (buf_) {
      buf_->trimEnd(n);
    }
  }

  // Prepend data (O(1) if headroom available)
  void prepend(size_t n) {
    ensureUnique();
    if (buf_) {
      buf_->prepend(n);
    }
  }

  // Get headroom (bytes available before data)
  size_t headroom() const {
    return buf_ ? buf_->headroom() : 0;
  }

  // Get tailroom (bytes available after data)
  size_t tailroom() const {
    return buf_ ? buf_->tailroom() : 0;
  }

  // Check if buffer is shared (refcount > 1)
  bool isShared() const {
    return buf_ && buf_.use_count() > 1;
  }

  // --- Factory methods ---

  static std::unique_ptr<Payload> create(size_t capacity) {
    return std::make_unique<Payload>(std::make_shared<ByteBuffer>(capacity));
  }

  static std::unique_ptr<Payload> copyBuffer(const void* data, size_t size) {
    auto buf = std::make_shared<ByteBuffer>(size);
    std::memcpy(buf->writableData(), data, size);
    buf->append(size);
    return std::make_unique<Payload>(std::move(buf));
  }

  static std::unique_ptr<Payload> copyBuffer(const std::string& str) {
    return copyBuffer(str.data(), str.size());
  }

  // Wrap a ByteBuffer (takes ownership, no copy!)
  static std::unique_ptr<Payload> wrap(std::unique_ptr<ByteBuffer> buf) {
    if (!buf) {
      return nullptr;
    }
    return std::make_unique<Payload>(std::move(buf));
  }

 private:
  // Ensure we have exclusive ownership before modifying
  void ensureUnique() {
    if (buf_ && buf_.use_count() > 1) {
      // Copy-on-write: make our own copy via clone()
      buf_ = std::shared_ptr<ByteBuffer>(buf_->clone());
    }
  }

  std::shared_ptr<ByteBuffer> buf_;
  std::unique_ptr<Payload> next_;  // For buffer chains
};

#endif // MOXYGEN_USE_FOLLY

} // namespace moxygen::compat

namespace moxygen {

// Helper function to create Payload from IOBuf (Folly mode) or ByteBuffer (std mode)
// This abstracts away the type difference between modes
#if MOXYGEN_USE_FOLLY
// In Folly mode, Payload is std::unique_ptr<folly::IOBuf>, so just return the IOBuf
inline std::unique_ptr<folly::IOBuf> toPayload(std::unique_ptr<folly::IOBuf> buf) {
  return buf;
}
#else
// In std mode, Payload is std::unique_ptr<compat::Payload>
inline std::unique_ptr<compat::Payload> toPayload(std::unique_ptr<compat::ByteBuffer> buf) {
  return compat::Payload::wrap(std::move(buf));
}
#endif

} // namespace moxygen
