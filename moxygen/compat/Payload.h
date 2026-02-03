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

#if MOXYGEN_USE_FOLLY
#include <folly/io/IOBuf.h>
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

// Std-mode Payload backed by vector
class Payload {
 public:
  Payload() = default;
  explicit Payload(std::vector<uint8_t> data) : data_(std::move(data)) {}

  // Move-only
  Payload(Payload&&) = default;
  Payload& operator=(Payload&&) = default;
  Payload(const Payload&) = delete;
  Payload& operator=(const Payload&) = delete;

  explicit operator bool() const { return !data_.empty(); }

  size_t computeChainDataLength() const { return data_.size(); }
  size_t length() const { return data_.size(); }
  const uint8_t* data() const { return data_.data(); }
  uint8_t* writableData() { return data_.data(); }
  bool empty() const { return data_.empty(); }
  void coalesce() {} // No-op for vector

  std::unique_ptr<Payload> clone() const {
    return std::make_unique<Payload>(data_);
  }

  std::string moveToString() {
    return std::string(
        reinterpret_cast<const char*>(data_.data()), data_.size());
  }

  std::string toString() const {
    return std::string(
        reinterpret_cast<const char*>(data_.data()), data_.size());
  }

  // No IOBuf in std mode
  void* getIOBuf() { return nullptr; }
  const void* getIOBuf() const { return nullptr; }

  static std::unique_ptr<Payload> create(size_t capacity) {
    auto p = std::make_unique<Payload>();
    p->data_.reserve(capacity);
    return p;
  }

  static std::unique_ptr<Payload> copyBuffer(const void* data, size_t size) {
    return std::make_unique<Payload>(std::vector<uint8_t>(
        static_cast<const uint8_t*>(data),
        static_cast<const uint8_t*>(data) + size));
  }

  static std::unique_ptr<Payload> copyBuffer(const std::string& str) {
    return copyBuffer(str.data(), str.size());
  }

 private:
  std::vector<uint8_t> data_;
};

#endif // MOXYGEN_USE_FOLLY

} // namespace moxygen::compat
