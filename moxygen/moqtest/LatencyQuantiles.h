/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace moxygen {

// Log-linear latency histogram for quantiles: 1 ms buckets below 128 ms, then
// 32 buckets per power of two (each at most 1/32 of its lower bound wide) up
// to 16384 ms. Larger values share one overflow bucket. Buckets are reported
// by the largest value they hold; the overflow bucket reports 16384.
class LatencyQuantiles {
 public:
  static constexpr unsigned kExactBits = 7;
  static constexpr unsigned kSubBucketBits = 5;
  static constexpr unsigned kMaxBits = 14;
  static constexpr uint64_t kExactLimitMs = uint64_t{1} << kExactBits;
  static constexpr uint64_t kSubBuckets = uint64_t{1} << kSubBucketBits;
  static constexpr uint64_t kMaxTrackedMs = uint64_t{1} << kMaxBits;
  static constexpr size_t kNumBuckets =
      kExactLimitMs + (kMaxBits - kExactBits) * kSubBuckets + 1;
  static constexpr size_t kOverflowBucket = kNumBuckets - 1;

  static size_t bucketIndex(uint64_t latencyMs) {
    if (latencyMs < kExactLimitMs) {
      return latencyMs;
    }
    if (latencyMs >= kMaxTrackedMs) {
      return kOverflowBucket;
    }
    unsigned exp = std::bit_width(latencyMs) - 1;
    uint64_t sub = (latencyMs >> (exp - kSubBucketBits)) - kSubBuckets;
    return kExactLimitMs + (exp - kExactBits) * kSubBuckets + sub;
  }

  static uint64_t bucketLowerMs(size_t i) {
    if (i < kExactLimitMs) {
      return i;
    }
    if (i >= kOverflowBucket) {
      return kMaxTrackedMs;
    }
    size_t j = i - kExactLimitMs;
    unsigned exp = kExactBits + j / kSubBuckets;
    return (kSubBuckets + j % kSubBuckets) << (exp - kSubBucketBits);
  }

  static uint64_t bucketUpperMs(size_t i) {
    if (i < kExactLimitMs || i >= kOverflowBucket) {
      return bucketLowerMs(i);
    }
    return bucketLowerMs(i + 1) - 1;
  }

  void addValue(uint64_t latencyMs) {
    addBucket(bucketIndex(latencyMs), 1);
  }

  void addBucket(size_t i, uint64_t n) {
    buckets_[i] += n;
    count_ += n;
  }

  void merge(const LatencyQuantiles& other) {
    for (size_t i = 0; i < kNumBuckets; ++i) {
      buckets_[i] += other.buckets_[i];
    }
    count_ += other.count_;
  }

  uint64_t count() const {
    return count_;
  }

  // Nearest-rank quantile, q in [0, 1]: the bucket holding the
  // ceil(q * count)-th smallest value.
  std::optional<uint64_t> quantile(double q) const {
    if (count_ == 0) {
      return std::nullopt;
    }
    // The epsilon keeps q * count from rounding up past an exact integer.
    double target = std::ceil(q * static_cast<double>(count_) - 1e-9);
    auto rank = static_cast<uint64_t>(
        std::clamp(target, 1.0, static_cast<double>(count_)));
    uint64_t running = 0;
    for (size_t i = 0; i < kNumBuckets; ++i) {
      running += buckets_[i];
      if (running >= rank) {
        return bucketUpperMs(i);
      }
    }
    return bucketUpperMs(kOverflowBucket);
  }

  // Non-empty buckets as space-separated "<ms>:<count>" pairs.
  std::string toString() const {
    std::string out;
    for (size_t i = 0; i < kNumBuckets; ++i) {
      if (buckets_[i] == 0) {
        continue;
      }
      if (!out.empty()) {
        out += ' ';
      }
      out += std::to_string(bucketUpperMs(i));
      out += ':';
      out += std::to_string(buckets_[i]);
    }
    return out;
  }

 private:
  std::array<uint64_t, kNumBuckets> buckets_{};
  uint64_t count_{0};
};

// Thread-safe accumulator; the reader drains one interval per takeInterval().
class AtomicLatencyQuantiles {
 public:
  void record(uint64_t latencyMs) {
    buckets_[LatencyQuantiles::bucketIndex(latencyMs)].fetch_add(
        1, std::memory_order_relaxed);
  }

  LatencyQuantiles takeInterval() {
    LatencyQuantiles out;
    for (size_t i = 0; i < buckets_.size(); ++i) {
      auto n = buckets_[i].exchange(0, std::memory_order_relaxed);
      if (n > 0) {
        out.addBucket(i, n);
      }
    }
    return out;
  }

 private:
  std::array<std::atomic<uint64_t>, LatencyQuantiles::kNumBuckets> buckets_{};
};

} // namespace moxygen
