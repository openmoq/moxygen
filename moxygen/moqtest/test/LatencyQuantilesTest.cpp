/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "moxygen/moqtest/LatencyQuantiles.h"
#include <folly/portability/GTest.h>
#include <limits>

using moxygen::AtomicLatencyQuantiles;
using moxygen::LatencyQuantiles;

TEST(LatencyQuantilesTest, BucketIndex) {
  EXPECT_EQ(LatencyQuantiles::bucketIndex(0), 0);
  EXPECT_EQ(LatencyQuantiles::bucketIndex(127), 127);
  EXPECT_EQ(LatencyQuantiles::bucketIndex(128), 128);
  EXPECT_EQ(LatencyQuantiles::bucketIndex(131), 128);
  EXPECT_EQ(LatencyQuantiles::bucketIndex(132), 129);
  EXPECT_EQ(LatencyQuantiles::bucketIndex(255), 159);
  EXPECT_EQ(LatencyQuantiles::bucketIndex(256), 160);
  EXPECT_EQ(
      LatencyQuantiles::bucketIndex(16383),
      LatencyQuantiles::kOverflowBucket - 1);
  EXPECT_EQ(
      LatencyQuantiles::bucketIndex(16384), LatencyQuantiles::kOverflowBucket);
  EXPECT_EQ(
      LatencyQuantiles::bucketIndex(std::numeric_limits<uint64_t>::max()),
      LatencyQuantiles::kOverflowBucket);
}

TEST(LatencyQuantilesTest, BucketsAreContiguousAndNarrow) {
  for (size_t i = 0; i < LatencyQuantiles::kOverflowBucket; ++i) {
    auto lower = LatencyQuantiles::bucketLowerMs(i);
    auto upper = LatencyQuantiles::bucketUpperMs(i);
    EXPECT_EQ(LatencyQuantiles::bucketIndex(lower), i);
    EXPECT_EQ(LatencyQuantiles::bucketIndex(upper), i);
    EXPECT_EQ(LatencyQuantiles::bucketLowerMs(i + 1), upper + 1);
    if (lower >= LatencyQuantiles::kExactLimitMs) {
      EXPECT_LE(upper - lower + 1, lower / LatencyQuantiles::kSubBuckets);
    } else {
      EXPECT_EQ(lower, upper);
    }
  }
}

TEST(LatencyQuantilesTest, EmptyHasNoQuantile) {
  LatencyQuantiles q;
  EXPECT_EQ(q.count(), 0);
  EXPECT_FALSE(q.quantile(0.5).has_value());
  EXPECT_EQ(q.toString(), "");
}

TEST(LatencyQuantilesTest, ExactRangeNearestRank) {
  LatencyQuantiles q;
  for (uint64_t v = 1; v <= 100; ++v) {
    q.addValue(v);
  }
  EXPECT_EQ(q.count(), 100);
  EXPECT_EQ(*q.quantile(0.0), 1);
  EXPECT_EQ(*q.quantile(0.07), 7);
  EXPECT_EQ(*q.quantile(0.5), 50);
  EXPECT_EQ(*q.quantile(0.9), 90);
  EXPECT_EQ(*q.quantile(0.95), 95);
  EXPECT_EQ(*q.quantile(0.99), 99);
  EXPECT_EQ(*q.quantile(1.0), 100);
}

TEST(LatencyQuantilesTest, RankRoundingAtLargeCount) {
  // 0.07 * 280037900 evaluates to 19602653.000000004.
  LatencyQuantiles q;
  q.addBucket(1, 19602653);
  q.addBucket(2, 280037900 - 19602653);
  EXPECT_EQ(*q.quantile(0.07), 1);
}

TEST(LatencyQuantilesTest, TailInLogRange) {
  LatencyQuantiles q;
  for (int i = 0; i < 990; ++i) {
    q.addValue(10);
  }
  for (int i = 0; i < 10; ++i) {
    q.addValue(1000);
  }
  EXPECT_EQ(*q.quantile(0.99), 10);
  auto p999 = *q.quantile(0.999);
  EXPECT_GE(p999, 1000);
  EXPECT_LE(p999, 1000 + 1000 / LatencyQuantiles::kSubBuckets);
}

TEST(LatencyQuantilesTest, OverflowReportsMaxTracked) {
  LatencyQuantiles q;
  q.addValue(20000);
  EXPECT_EQ(*q.quantile(0.5), LatencyQuantiles::kMaxTrackedMs);
  EXPECT_EQ(q.toString(), "16384:1");
}

TEST(LatencyQuantilesTest, ToStringUsesBucketUpperBound) {
  LatencyQuantiles q;
  q.addValue(3);
  q.addValue(3);
  q.addValue(200);
  EXPECT_EQ(q.toString(), "3:2 203:1");
}

TEST(LatencyQuantilesTest, Merge) {
  LatencyQuantiles a;
  LatencyQuantiles b;
  a.addValue(5);
  b.addValue(7);
  b.addValue(9);
  a.merge(b);
  EXPECT_EQ(a.count(), 3);
  EXPECT_EQ(*a.quantile(0.5), 7);
  EXPECT_EQ(a.toString(), "5:1 7:1 9:1");
}

TEST(LatencyQuantilesTest, AtomicTakeIntervalDrains) {
  AtomicLatencyQuantiles atomic;
  atomic.record(4);
  atomic.record(4);
  atomic.record(300);
  auto first = atomic.takeInterval();
  EXPECT_EQ(first.count(), 3);
  EXPECT_EQ(first.toString(), "4:2 303:1");
  EXPECT_EQ(atomic.takeInterval().count(), 0);
  atomic.record(1);
  EXPECT_EQ(atomic.takeInterval().toString(), "1:1");
}
