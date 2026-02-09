/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <moxygen/events/MoQExecutor.h>

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <vector>

namespace moxygen {

/**
 * MoQSimpleExecutor - A simple executor for std-mode (no Folly dependency).
 *
 * Provides a single-threaded event loop with task queuing and timeout
 * scheduling, using mutex + condition_variable for wakeup.
 *
 * Usage:
 *   auto executor = std::make_shared<MoQSimpleExecutor>();
 *   executor->add([]() { doWork(); });
 *   executor->scheduleTimeout([]() { doDelayedWork(); }, 1000ms);
 *   executor->run();  // Blocks until stop() is called
 */
class MoQSimpleExecutor : public MoQExecutor {
 public:
  MoQSimpleExecutor() = default;
  ~MoQSimpleExecutor() override;

  // MoQExecutor interface
  void add(std::function<void()> func) override;
  void scheduleTimeout(
      std::function<void()> callback,
      std::chrono::milliseconds delay) override;

  // Run the event loop (blocks until stop() is called)
  void run();

  // Run one iteration of the event loop (non-blocking)
  // Returns true if work was done
  bool runOnce();

  // Run the event loop for a specified duration
  void runFor(std::chrono::milliseconds duration);

  // Schedule a callback at a specific time point
  void scheduleAt(
      std::function<void()> callback,
      std::chrono::steady_clock::time_point deadline);

  // Signal the event loop to stop
  void stop();

  // Check if the executor is running
  bool isRunning() const {
    return running_;
  }

 private:
  struct TimerEntry {
    std::chrono::steady_clock::time_point deadline;
    std::function<void()> callback;

    bool operator>(const TimerEntry& other) const {
      return deadline > other.deadline;
    }
  };

  std::mutex mutex_;
  std::condition_variable cv_;
  std::queue<std::function<void()>> tasks_;
  std::priority_queue<TimerEntry, std::vector<TimerEntry>, std::greater<>>
      timers_;
  bool running_{false};
  bool stopping_{false};
};

} // namespace moxygen
