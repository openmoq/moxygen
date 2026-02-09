/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

// Test executor abstraction that works across all build modes
// Provides a unified interface for running async operations in tests

#include <moxygen/events/MoQExecutor.h>

#include <chrono>
#include <functional>
#include <memory>

#if MOXYGEN_USE_FOLLY && defined(MOXYGEN_QUIC_MVFST)
#include <folly/io/async/EventBase.h>
#include <moxygen/events/MoQFollyExecutorImpl.h>
#else
#include <moxygen/events/MoQSimpleExecutor.h>
#endif

namespace moxygen::test {

/**
 * TestExecutor provides a unified interface for test execution across modes.
 *
 * In Folly + mvfst mode: Uses EventBase for event loop
 * In std-mode or Folly + picoquic: Uses MoQSimpleExecutor
 */
class TestExecutor {
 public:
  TestExecutor();
  ~TestExecutor();

  // Get the underlying executor for scheduling work
  MoQExecutor::KeepAlive getKeepAlive();

  // Run the event loop for a specified duration
  void runFor(std::chrono::milliseconds duration);

  // Run until a condition is met or timeout occurs
  // Returns true if condition was met, false on timeout
  template <typename Predicate>
  bool runUntil(Predicate pred, std::chrono::milliseconds timeout) {
    auto start = std::chrono::steady_clock::now();
    while (!pred()) {
      auto elapsed = std::chrono::steady_clock::now() - start;
      if (elapsed >= timeout) {
        return false;
      }
      runOnce();
    }
    return true;
  }

  // Run one iteration of the event loop
  void runOnce();

  // Stop the executor
  void stop();

  // Schedule a function to run
  void schedule(std::function<void()> fn);

  // Schedule a function to run after a delay
  void scheduleAfter(std::function<void()> fn, std::chrono::milliseconds delay);

 private:
#if MOXYGEN_USE_FOLLY && defined(MOXYGEN_QUIC_MVFST)
  folly::EventBase eventBase_;
  std::unique_ptr<MoQFollyExecutorImpl> executor_;
#else
  std::shared_ptr<MoQSimpleExecutor> executor_;
#endif
};

// Implementation

inline TestExecutor::TestExecutor() {
#if MOXYGEN_USE_FOLLY && defined(MOXYGEN_QUIC_MVFST)
  executor_ = std::make_unique<MoQFollyExecutorImpl>(&eventBase_);
#else
  executor_ = std::make_shared<MoQSimpleExecutor>();
#endif
}

inline TestExecutor::~TestExecutor() {
  stop();
}

inline MoQExecutor::KeepAlive TestExecutor::getKeepAlive() {
#if MOXYGEN_USE_FOLLY && defined(MOXYGEN_QUIC_MVFST)
  return executor_->getKeepAlive();
#else
  return executor_;
#endif
}

inline void TestExecutor::runFor(std::chrono::milliseconds duration) {
#if MOXYGEN_USE_FOLLY && defined(MOXYGEN_QUIC_MVFST)
  eventBase_.loopOnce();
  // Run for approximately the duration
  auto start = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - start < duration) {
    eventBase_.loopOnce(EVLOOP_NONBLOCK);
  }
#else
  executor_->runFor(duration);
#endif
}

inline void TestExecutor::runOnce() {
#if MOXYGEN_USE_FOLLY && defined(MOXYGEN_QUIC_MVFST)
  eventBase_.loopOnce(EVLOOP_NONBLOCK);
#else
  executor_->runOnce();
#endif
}

inline void TestExecutor::stop() {
#if MOXYGEN_USE_FOLLY && defined(MOXYGEN_QUIC_MVFST)
  eventBase_.terminateLoopSoon();
#else
  executor_->stop();
#endif
}

inline void TestExecutor::schedule(std::function<void()> fn) {
#if MOXYGEN_USE_FOLLY && defined(MOXYGEN_QUIC_MVFST)
  eventBase_.runInEventBaseThread(std::move(fn));
#else
  executor_->add(std::move(fn));
#endif
}

inline void TestExecutor::scheduleAfter(
    std::function<void()> fn,
    std::chrono::milliseconds delay) {
#if MOXYGEN_USE_FOLLY && defined(MOXYGEN_QUIC_MVFST)
  eventBase_.scheduleAt(std::move(fn), std::chrono::steady_clock::now() + delay);
#else
  executor_->scheduleAt(std::move(fn), std::chrono::steady_clock::now() + delay);
#endif
}

} // namespace moxygen::test
