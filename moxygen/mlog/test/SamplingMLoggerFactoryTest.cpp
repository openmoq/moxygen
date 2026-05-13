/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <moxygen/mlog/MLogger.h>
#include <moxygen/mlog/MLoggerFactory.h>
#include <moxygen/mlog/SamplingMLoggerFactory.h>

#include <folly/portability/GTest.h>
#include <memory>

namespace moxygen {

// ---------------------------------------------------------------------------
// Minimal spy factory: counts createMLogger() calls, returns real loggers
// ---------------------------------------------------------------------------

class SpyMLoggerFactory : public MLoggerFactory {
 public:
  int callCount{0};

  std::shared_ptr<MLogger> createMLogger() override;
};

// Minimal no-op MLogger for testing — outputLogs() does nothing
class NullMLogger : public MLogger {
 public:
  explicit NullMLogger() : MLogger(VantagePoint::SERVER) {}
  void outputLogs() override {}
};

std::shared_ptr<MLogger> SpyMLoggerFactory::createMLogger() {
  ++callCount;
  return std::make_shared<NullMLogger>();
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

class SamplingMLoggerFactoryTest : public ::testing::Test {
 protected:
  std::shared_ptr<SpyMLoggerFactory> inner_ =
      std::make_shared<SpyMLoggerFactory>();
};

// rate <= 0: always returns nullptr, inner never called
TEST_F(SamplingMLoggerFactoryTest, ZeroRate_AlwaysReturnsNull) {
  constexpr float kZeroRate = 0.0f;
  constexpr int kBasicTrials = 100;
  SamplingMLoggerFactory factory(inner_, kZeroRate);
  for (int i = 0; i < kBasicTrials; ++i) {
    EXPECT_EQ(factory.createMLogger(), nullptr);
  }
  EXPECT_EQ(inner_->callCount, 0);
}

// rate >= 1.0: always returns a logger, inner always called
TEST_F(SamplingMLoggerFactoryTest, FullRate_AlwaysReturnsLogger) {
  constexpr float kFullRate = 1.0f;
  constexpr int kBasicTrials = 100;
  SamplingMLoggerFactory factory(inner_, kFullRate);
  for (int i = 0; i < kBasicTrials; ++i) {
    EXPECT_NE(factory.createMLogger(), nullptr);
  }
  EXPECT_EQ(inner_->callCount, kBasicTrials);
}

// rate = 0.5: roughly 50% of calls should produce a logger.
// Use a large sample to stay within a wide tolerance (30–70%) with
// overwhelming probability, avoiding flakiness.
TEST_F(SamplingMLoggerFactoryTest, HalfRate_ApproximatelyHalfSampled) {
  constexpr float kHalfSampleRate = 0.5f;
  constexpr int kStatisticalTrials = 2000;
  constexpr float kToleranceLow = 0.30f;
  constexpr float kToleranceHigh = 0.70f;
  SamplingMLoggerFactory factory(inner_, kHalfSampleRate);
  int logged = 0;
  for (int i = 0; i < kStatisticalTrials; ++i) {
    if (factory.createMLogger() != nullptr) {
      ++logged;
    }
  }
  // Expect 50% ± 20% — this fails with probability < 1e-20 for a fair coin
  EXPECT_GT(logged, kStatisticalTrials * kToleranceLow);
  EXPECT_LT(logged, kStatisticalTrials * kToleranceHigh);
}

// rate = 0.01: ~1% sampling — should produce some loggers and some nulls
TEST_F(SamplingMLoggerFactoryTest, OnePercentRate_SamplesOccasionally) {
  constexpr float kOnePercentRate = 0.01f;
  constexpr int kHighVolTrials = 5000;
  SamplingMLoggerFactory factory(inner_, kOnePercentRate);
  int logged = 0;
  for (int i = 0; i < kHighVolTrials; ++i) {
    if (factory.createMLogger() != nullptr) {
      ++logged;
    }
  }
  // Expect at least 1 log and far fewer than all
  EXPECT_GT(logged, 0);
  EXPECT_LT(logged, kHighVolTrials);
}

// Verify the inner factory is only called for sampled sessions
TEST_F(SamplingMLoggerFactoryTest, InnerCalledOnlyForSampledSessions) {
  constexpr float kHalfSampleRate = 0.5f;
  constexpr int kTrials = 500;
  SamplingMLoggerFactory factory(inner_, kHalfSampleRate);
  int logged = 0;
  for (int i = 0; i < kTrials; ++i) {
    if (factory.createMLogger() != nullptr) {
      ++logged;
    }
  }
  EXPECT_EQ(inner_->callCount, logged);
}

// Returned loggers are valid (not null) and usable
TEST_F(SamplingMLoggerFactoryTest, ReturnedLoggerIsUsable) {
  constexpr float kFullRate = 1.0f;
  SamplingMLoggerFactory factory(inner_, kFullRate);
  auto logger = factory.createMLogger();
  ASSERT_NE(logger, nullptr);
  EXPECT_NO_THROW(logger->outputLogs());
}

} // namespace moxygen
