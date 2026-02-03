/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <moxygen/compat/Config.h>

#if MOXYGEN_USE_FOLLY

#include <moxygen/compat/IOBufPayload.h>
#include <moxygen/compat/PayloadImpl.h>

namespace moxygen::compat {

IOBufPayload::IOBufPayload(std::unique_ptr<folly::IOBuf> buf)
    : buf_(std::move(buf)) {}

std::vector<std::span<const uint8_t>> IOBufPayload::spans() const {
  std::vector<std::span<const uint8_t>> result;
  if (!buf_) {
    return result;
  }
  // IOBuf can be a chain, iterate through all buffers
  const folly::IOBuf* current = buf_.get();
  do {
    if (current->length() > 0) {
      result.emplace_back(current->data(), current->length());
    }
    current = current->next();
  } while (current != buf_.get());
  return result;
}

size_t IOBufPayload::totalSize() const {
  return buf_ ? buf_->computeChainDataLength() : 0;
}

bool IOBufPayload::empty() const {
  return !buf_ || buf_->computeChainDataLength() == 0;
}

std::unique_ptr<Payload> IOBufPayload::clone() const {
  if (!buf_) {
    return std::make_unique<IOBufPayload>();
  }
  return std::make_unique<IOBufPayload>(buf_->clone());
}

std::unique_ptr<folly::IOBuf> payloadToIOBuf(std::unique_ptr<Payload> payload) {
  if (!payload) {
    return folly::IOBuf::create(0);
  }

  switch (payload->type()) {
    case PayloadType::IOBuf: {
      // Zero-copy: move the IOBuf out
      return static_cast<IOBufPayload*>(payload.get())->releaseIOBuf();
    }
    case PayloadType::Vector: {
      // Zero-copy: take ownership of vector's buffer
      auto* vecPayload = static_cast<VectorPayload*>(payload.get());
      auto vec = vecPayload->releaseData();
      if (vec.empty()) {
        return folly::IOBuf::create(0);
      }
      // Transfer vector ownership to IOBuf via custom deleter
      auto* data = vec.data();
      auto size = vec.size();
      auto* vecPtr = new std::vector<uint8_t>(std::move(vec));
      return folly::IOBuf::takeOwnership(
          data,
          size,
          [](void*, void* userData) {
            delete static_cast<std::vector<uint8_t>*>(userData);
          },
          vecPtr);
    }
    case PayloadType::Shared: {
      // Zero-copy: share ownership via shared_ptr custom deleter
      auto* sharedPayload = static_cast<SharedPayload*>(payload.get());
      auto [data, size] = sharedPayload->releaseData();
      if (!data || size == 0) {
        return folly::IOBuf::create(0);
      }
      // Keep shared_ptr alive via custom deleter
      auto* sharedPtr = new std::shared_ptr<uint8_t[]>(std::move(data));
      return folly::IOBuf::takeOwnership(
          sharedPtr->get(),
          size,
          [](void*, void* userData) {
            delete static_cast<std::shared_ptr<uint8_t[]>*>(userData);
          },
          sharedPtr);
    }
  }
  // Should never reach here
  return folly::IOBuf::create(0);
}

std::unique_ptr<Payload> iobufToPayload(std::unique_ptr<folly::IOBuf> buf) {
  return std::make_unique<IOBufPayload>(std::move(buf));
}

} // namespace moxygen::compat

#endif // MOXYGEN_USE_FOLLY
