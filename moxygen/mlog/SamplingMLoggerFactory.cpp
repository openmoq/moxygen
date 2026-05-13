/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "moxygen/mlog/SamplingMLoggerFactory.h"

namespace moxygen {

SamplingMLoggerFactory::SamplingMLoggerFactory(
    std::shared_ptr<MLoggerFactory> inner,
    float sampleRate)
    : inner_(std::move(inner)), sampleRate_(sampleRate) {
  // bucketSize = how many sessions per one logged session, e.g. 0.01 -> 100
  bucketSize_ = static_cast<uint32_t>(1.0f / sampleRate_);
  if (bucketSize_ == 0) {
    bucketSize_ = 1; // guard against rounding to zero for very high rates
  }
}

std::shared_ptr<MLogger> SamplingMLoggerFactory::createMLogger() {
  if (sampleRate_ <= 0.0f) {
    return nullptr;
  }
  if (sampleRate_ >= 1.0f) {
    return inner_->createMLogger();
  }
  // folly::Random::oneIn() is thread-safe.
  if (folly::Random::oneIn(bucketSize_)) {
    return inner_->createMLogger();
  }
  return nullptr;
}

} // namespace moxygen
