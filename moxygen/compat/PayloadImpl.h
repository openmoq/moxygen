/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <moxygen/compat/Payload.h>

namespace moxygen::compat {

// Payload backed by std::vector<uint8_t> (pure std, no folly)
class VectorPayload : public Payload {
 public:
  VectorPayload() = default;
  explicit VectorPayload(std::vector<uint8_t> data);
  VectorPayload(const void* data, size_t size);

  PayloadType type() const override { return PayloadType::Vector; }
  std::vector<std::span<const uint8_t>> spans() const override;
  size_t totalSize() const override;
  bool empty() const override;
  std::unique_ptr<Payload> clone() const override;

  // Access underlying data
  const std::vector<uint8_t>& data() const { return data_; }

  // Move data out (destructive)
  std::vector<uint8_t> releaseData() { return std::move(data_); }

 private:
  std::vector<uint8_t> data_;
};

// Payload backed by shared_ptr<uint8_t[]> (for shared ownership scenarios)
class SharedPayload : public Payload {
 public:
  SharedPayload() = default;
  SharedPayload(std::shared_ptr<uint8_t[]> data, size_t size);

  PayloadType type() const override { return PayloadType::Shared; }
  std::vector<std::span<const uint8_t>> spans() const override;
  size_t totalSize() const override;
  bool empty() const override;
  std::unique_ptr<Payload> clone() const override;

  // Access underlying data
  const uint8_t* data() const { return data_.get(); }
  size_t size() const { return size_; }

  // Release ownership for zero-copy conversion
  std::pair<std::shared_ptr<uint8_t[]>, size_t> releaseData() {
    return {std::move(data_), size_};
  }

 private:
  std::shared_ptr<uint8_t[]> data_;
  size_t size_{0};
};

// Factory functions
inline std::unique_ptr<Payload> makeVectorPayload(std::vector<uint8_t> data) {
  return std::make_unique<VectorPayload>(std::move(data));
}

inline std::unique_ptr<Payload> makeVectorPayload(const void* data, size_t size) {
  return std::make_unique<VectorPayload>(data, size);
}

inline std::unique_ptr<Payload> makeSharedPayload(
    std::shared_ptr<uint8_t[]> data,
    size_t size) {
  return std::make_unique<SharedPayload>(std::move(data), size);
}

} // namespace moxygen::compat
