/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace moxygen::compat {

// Type enum for efficient type checking without dynamic_cast
enum class PayloadType { Vector, Shared, IOBuf };

// Abstract payload interface - completely folly-free
class Payload {
 public:
  virtual ~Payload() = default;

  // Type identification
  virtual PayloadType type() const = 0;

  // Read access via spans (one span per buffer segment)
  virtual std::vector<std::span<const uint8_t>> spans() const = 0;

  // Total size across all spans
  virtual size_t totalSize() const = 0;

  // Check if empty
  virtual bool empty() const = 0;

  // Clone the payload
  virtual std::unique_ptr<Payload> clone() const = 0;

  // Coalesce all data into a single contiguous vector
  std::vector<uint8_t> coalesce() const {
    std::vector<uint8_t> result;
    result.reserve(totalSize());
    for (const auto& span : spans()) {
      result.insert(result.end(), span.begin(), span.end());
    }
    return result;
  }
};

} // namespace moxygen::compat
