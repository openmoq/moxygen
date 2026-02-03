/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <moxygen/compat/Config.h>

#if MOXYGEN_USE_FOLLY

#include <folly/io/IOBuf.h>
#include <moxygen/compat/Payload.h>

namespace moxygen::compat {

// Payload backed by folly::IOBuf - zero-copy wrapper
class IOBufPayload : public Payload {
 public:
  IOBufPayload() = default;
  explicit IOBufPayload(std::unique_ptr<folly::IOBuf> buf);

  PayloadType type() const override { return PayloadType::IOBuf; }
  std::vector<std::span<const uint8_t>> spans() const override;
  size_t totalSize() const override;
  bool empty() const override;
  std::unique_ptr<Payload> clone() const override;

  // IOBuf-specific access for adapter layer
  folly::IOBuf* iobuf() const { return buf_.get(); }
  std::unique_ptr<folly::IOBuf> releaseIOBuf() { return std::move(buf_); }

 private:
  std::unique_ptr<folly::IOBuf> buf_;
};

// Factory function
inline std::unique_ptr<Payload> makeIOBufPayload(
    std::unique_ptr<folly::IOBuf> buf) {
  return std::make_unique<IOBufPayload>(std::move(buf));
}

// Conversion utilities - destructive, takes ownership
std::unique_ptr<folly::IOBuf> payloadToIOBuf(std::unique_ptr<Payload> payload);
std::unique_ptr<Payload> iobufToPayload(std::unique_ptr<folly::IOBuf> buf);

} // namespace moxygen::compat

#endif // MOXYGEN_USE_FOLLY
