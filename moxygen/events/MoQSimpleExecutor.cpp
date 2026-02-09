/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "moxygen/events/MoQSimpleExecutor.h"

#include <thread>

namespace moxygen {

MoQSimpleExecutor::~MoQSimpleExecutor() {
  stop();
}

void MoQSimpleExecutor::add(std::function<void()> func) {
  if (!func) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    tasks_.push(std::move(func));
  }
  cv_.notify_one();
}

void MoQSimpleExecutor::scheduleTimeout(
    std::function<void()> callback,
    std::chrono::milliseconds delay) {
  if (!callback) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    timers_.push(
        {std::chrono::steady_clock::now() + delay, std::move(callback)});
  }
  cv_.notify_one();
}

void MoQSimpleExecutor::run() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = true;
    stopping_ = false;
  }

  while (true) {
    std::function<void()> task;

    {
      std::unique_lock<std::mutex> lock(mutex_);

      // Determine wait deadline from the next timer
      auto waitUntil = std::chrono::steady_clock::time_point::max();
      if (!timers_.empty()) {
        waitUntil = timers_.top().deadline;
      }

      // Wait for tasks, timers, or stop signal
      if (tasks_.empty() && !stopping_) {
        if (waitUntil == std::chrono::steady_clock::time_point::max()) {
          cv_.wait(lock, [this]() { return !tasks_.empty() || stopping_ || !timers_.empty(); });
        } else {
          cv_.wait_until(lock, waitUntil, [this]() {
            return !tasks_.empty() || stopping_;
          });
        }
      }

      if (stopping_ && tasks_.empty()) {
        // Drain remaining timers that are due before exiting
        auto now = std::chrono::steady_clock::now();
        while (!timers_.empty() && timers_.top().deadline <= now) {
          auto timerCb = std::move(const_cast<TimerEntry&>(timers_.top()).callback);
          timers_.pop();
          lock.unlock();
          timerCb();
          lock.lock();
        }
        break;
      }

      // Fire any timers that are due
      auto now = std::chrono::steady_clock::now();
      while (!timers_.empty() && timers_.top().deadline <= now) {
        auto timerCb = std::move(const_cast<TimerEntry&>(timers_.top()).callback);
        timers_.pop();
        lock.unlock();
        timerCb();
        lock.lock();
        // Recheck time after executing callback
        now = std::chrono::steady_clock::now();
      }

      // Get next task if available
      if (!tasks_.empty()) {
        task = std::move(tasks_.front());
        tasks_.pop();
      }
    }

    // Execute task outside the lock
    if (task) {
      task();
    }
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = false;
  }
}

void MoQSimpleExecutor::stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
  }
  cv_.notify_one();
}

bool MoQSimpleExecutor::runOnce() {
  std::function<void()> task;
  bool didWork = false;

  {
    std::unique_lock<std::mutex> lock(mutex_);

    // Fire any timers that are due
    auto now = std::chrono::steady_clock::now();
    while (!timers_.empty() && timers_.top().deadline <= now) {
      auto timerCb = std::move(const_cast<TimerEntry&>(timers_.top()).callback);
      timers_.pop();
      lock.unlock();
      timerCb();
      didWork = true;
      lock.lock();
      now = std::chrono::steady_clock::now();
    }

    // Get next task if available
    if (!tasks_.empty()) {
      task = std::move(tasks_.front());
      tasks_.pop();
    }
  }

  // Execute task outside the lock
  if (task) {
    task();
    didWork = true;
  }

  return didWork;
}

void MoQSimpleExecutor::runFor(std::chrono::milliseconds duration) {
  auto deadline = std::chrono::steady_clock::now() + duration;
  while (std::chrono::steady_clock::now() < deadline) {
    if (!runOnce()) {
      // No work, sleep briefly to avoid busy-wait
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
}

void MoQSimpleExecutor::scheduleAt(
    std::function<void()> callback,
    std::chrono::steady_clock::time_point deadline) {
  if (!callback) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    timers_.push({deadline, std::move(callback)});
  }
  cv_.notify_one();
}

} // namespace moxygen
