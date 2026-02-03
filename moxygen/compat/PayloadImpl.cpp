/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <moxygen/compat/PayloadImpl.h>

namespace moxygen::compat {

// VectorPayload implementation

VectorPayload::VectorPayload(std::vector<uint8_t> data)
    : data_(std::move(data)) {}

VectorPayload::VectorPayload(const void* data, size_t size)
    : data_(static_cast<const uint8_t*>(data),
            static_cast<const uint8_t*>(data) + size) {}

std::vector<std::span<const uint8_t>> VectorPayload::spans() const {
  if (data_.empty()) {
    return {};
  }
  return {std::span<const uint8_t>(data_.data(), data_.size())};
}

size_t VectorPayload::totalSize() const {
  return data_.size();
}

bool VectorPayload::empty() const {
  return data_.empty();
}

std::unique_ptr<Payload> VectorPayload::clone() const {
  return std::make_unique<VectorPayload>(data_);
}

// SharedPayload implementation

SharedPayload::SharedPayload(std::shared_ptr<uint8_t[]> data, size_t size)
    : data_(std::move(data)), size_(size) {}

std::vector<std::span<const uint8_t>> SharedPayload::spans() const {
  if (!data_ || size_ == 0) {
    return {};
  }
  return {std::span<const uint8_t>(data_.get(), size_)};
}

size_t SharedPayload::totalSize() const {
  return size_;
}

bool SharedPayload::empty() const {
  return !data_ || size_ == 0;
}

std::unique_ptr<Payload> SharedPayload::clone() const {
  // Shares the underlying data
  return std::make_unique<SharedPayload>(data_, size_);
}

} // namespace moxygen::compat
