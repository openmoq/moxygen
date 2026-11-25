/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>

namespace moxygen {

// Run totals plus an interval min/avg/max that the aggregator drains each
// tick.  Safe to write and read from different threads.
class AtomicLatency {
 public:
  struct Interval {
    uint64_t sumMs{0};
    uint64_t count{0};
    uint64_t minMs{std::numeric_limits<uint64_t>::max()};
    uint64_t maxMs{0};

    void merge(const Interval& other) {
      sumMs += other.sumMs;
      count += other.count;
      minMs = std::min(minMs, other.minMs);
      maxMs = std::max(maxMs, other.maxMs);
    }
  };

  void record(uint64_t latencyMs) {
    sum_.fetch_add(latencyMs, std::memory_order_relaxed);
    count_.fetch_add(1, std::memory_order_relaxed);
    intervalSum_.fetch_add(latencyMs, std::memory_order_relaxed);
    intervalCount_.fetch_add(1, std::memory_order_relaxed);

    uint64_t cur = intervalMin_.load(std::memory_order_relaxed);
    while (latencyMs < cur &&
           !intervalMin_.compare_exchange_weak(
               cur, latencyMs, std::memory_order_relaxed)) {
    }
    cur = intervalMax_.load(std::memory_order_relaxed);
    while (latencyMs > cur &&
           !intervalMax_.compare_exchange_weak(
               cur, latencyMs, std::memory_order_relaxed)) {
    }
  }

  uint64_t sumMs() const {
    return sum_.load(std::memory_order_relaxed);
  }
  uint64_t count() const {
    return count_.load(std::memory_order_relaxed);
  }

  Interval takeInterval() {
    Interval ivl;
    ivl.sumMs = intervalSum_.exchange(0, std::memory_order_relaxed);
    ivl.count = intervalCount_.exchange(0, std::memory_order_relaxed);
    ivl.minMs = intervalMin_.exchange(
        std::numeric_limits<uint64_t>::max(), std::memory_order_relaxed);
    ivl.maxMs = intervalMax_.exchange(0, std::memory_order_relaxed);
    return ivl;
  }

 private:
  std::atomic<uint64_t> sum_{0};
  std::atomic<uint64_t> count_{0};
  std::atomic<uint64_t> intervalSum_{0};
  std::atomic<uint64_t> intervalCount_{0};
  std::atomic<uint64_t> intervalMin_{std::numeric_limits<uint64_t>::max()};
  std::atomic<uint64_t> intervalMax_{0};
};

} // namespace moxygen
