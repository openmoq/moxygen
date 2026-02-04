/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <moxygen/compat/Config.h>

#if MOXYGEN_USE_FOLLY && MOXYGEN_QUIC_MVFST
// When using mvfst, use the native ConnectionId type
#include <quic/codec/QuicConnectionId.h>

namespace moxygen::compat {
using ConnectionId = quic::ConnectionId;
} // namespace moxygen::compat

#else
// Standalone ConnectionId implementation for picoquic or std-mode

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace moxygen::compat {

/**
 * ConnectionId - QUIC connection identifier
 *
 * A connection ID is a variable-length identifier (0-20 bytes) used to
 * identify QUIC connections. This implementation provides a simple
 * byte-array-based representation compatible with picoquic.
 */
class ConnectionId {
 public:
  static constexpr size_t kMaxLength = 20;

  ConnectionId() : length_(0) {
    data_.fill(0);
  }

  explicit ConnectionId(const std::vector<uint8_t>& data) {
    length_ = std::min(data.size(), kMaxLength);
    std::memcpy(data_.data(), data.data(), length_);
  }

  ConnectionId(const uint8_t* data, size_t length) {
    length_ = std::min(length, kMaxLength);
    std::memcpy(data_.data(), data, length_);
  }

  const uint8_t* data() const {
    return data_.data();
  }

  size_t size() const {
    return length_;
  }

  bool empty() const {
    return length_ == 0;
  }

  bool operator==(const ConnectionId& other) const {
    return length_ == other.length_ &&
           std::memcmp(data_.data(), other.data_.data(), length_) == 0;
  }

  bool operator!=(const ConnectionId& other) const {
    return !(*this == other);
  }

  // Convert to hex string for logging
  std::string hex() const {
    static const char hexChars[] = "0123456789abcdef";
    std::string result;
    result.reserve(length_ * 2);
    for (size_t i = 0; i < length_; ++i) {
      result.push_back(hexChars[(data_[i] >> 4) & 0xF]);
      result.push_back(hexChars[data_[i] & 0xF]);
    }
    return result;
  }

 private:
  std::array<uint8_t, kMaxLength> data_;
  size_t length_;
};

} // namespace moxygen::compat

#endif // MOXYGEN_USE_FOLLY && MOXYGEN_QUIC_MVFST
