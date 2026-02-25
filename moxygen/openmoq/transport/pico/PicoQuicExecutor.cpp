/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "moxygen/openmoq/transport/pico/PicoQuicExecutor.h"
#include <algorithm>
#include <folly/logging/xlog.h>
#include <quic/common/events/QuicEventBase.h>

namespace moxygen {

PicoQuicExecutor::PicoQuicExecutor()
    : timers_([](const std::shared_ptr<TimerEntry> &a,
                 const std::shared_ptr<TimerEntry> &b) {
        return a->expiryTime > b->expiryTime; // min-heap
      }) {}

void PicoQuicExecutor::add(folly::Func f) {
  std::lock_guard<std::mutex> lock(taskMutex_);
  tasks_.push(std::move(f));
  XLOG(DBG6) << "PicoQuicExecutor::add: task added, queue size="
             << tasks_.size();
  // The packet loop's time_check callback will see hasPendingWork()
  // and set timeout to 0, waking up the loop
}

void PicoQuicExecutor::scheduleTimeout(quic::QuicTimerCallback *callback,
                                       std::chrono::milliseconds timeout) {
  uint64_t currentTime = picoquic_current_time();
  uint64_t expiryTime =
      currentTime + static_cast<uint64_t>(timeout.count()) * 1000;

  // Create shared timer entry
  auto entry = std::make_shared<TimerEntry>();
  entry->expiryTime = expiryTime;
  entry->callback = callback;
  entry->cancelled = false;

  // Create impl handle for cancellation support
  auto *implHandle = new TimerCallbackImplHandle(entry);
  quic::QuicEventBase::setImplHandle(callback, implHandle);

  std::lock_guard<std::mutex> lock(timerMutex_);
  timers_.push(entry);
}

void PicoQuicExecutor::drainTasks() {
  std::queue<folly::Func> localTasks;
  {
    std::lock_guard<std::mutex> lock(taskMutex_);
    std::swap(localTasks, tasks_);
  }

  size_t taskCount = localTasks.size();
  if (taskCount > 0) {
    XLOG(DBG4) << "PicoQuicExecutor::drainTasks: draining " << taskCount
               << " tasks";
  }

  while (!localTasks.empty()) {
    auto task = std::move(localTasks.front());
    localTasks.pop();
    try {
      task();
    } catch (const std::exception &ex) {
      XLOG(ERR) << "Exception in executor task: " << ex.what();
    }
  }

  if (taskCount > 0) {
    XLOG(DBG4) << "PicoQuicExecutor::drainTasks: completed " << taskCount
               << " tasks";
  }
}

void PicoQuicExecutor::processExpiredTimers(uint64_t currentTime) {
  std::vector<quic::QuicTimerCallback *> expiredCallbacks;

  {
    std::lock_guard<std::mutex> lock(timerMutex_);
    while (!timers_.empty() && timers_.top()->expiryTime <= currentTime) {
      auto entry = timers_.top();
      timers_.pop();

      // Only fire if not cancelled
      if (!entry->cancelled) {
        expiredCallbacks.push_back(entry->callback);
      }
    }
  }

  for (auto *callback : expiredCallbacks) {
    try {
      callback->timeoutExpired();
    } catch (const std::exception &ex) {
      XLOG(ERR) << "Exception in timer callback: " << ex.what();
    }
  }
}

bool PicoQuicExecutor::hasPendingWork() const {
  std::lock_guard<std::mutex> lock(taskMutex_);
  return !tasks_.empty();
}

int64_t PicoQuicExecutor::getNextTimeoutDelta(uint64_t currentTime) const {
  std::lock_guard<std::mutex> lock(timerMutex_);

  if (timers_.empty()) {
    return INT64_MAX; // No timers pending
  }

  uint64_t nextExpiry = timers_.top()->expiryTime;
  if (nextExpiry <= currentTime) {
    return 0; // Timer already expired
  }

  return static_cast<int64_t>(nextExpiry - currentTime);
}

int PicoQuicExecutor::loopCallbackStatic(picoquic_quic_t *quic,
                                         picoquic_packet_loop_cb_enum cb_mode,
                                         void *callback_ctx,
                                         void *callback_arg) {
  auto *self = static_cast<PicoQuicExecutor *>(callback_ctx);
  if (!self) {
    return 0;
  }
  return self->loopCallback(quic, cb_mode, callback_arg);
}

int PicoQuicExecutor::loopCallback(picoquic_quic_t * /*quic*/,
                                   picoquic_packet_loop_cb_enum cb_mode,
                                   void *callback_arg) {

  switch (cb_mode) {
  case picoquic_packet_loop_ready: {
    // Enable time check so we can control epoll timeout
    auto *options = static_cast<picoquic_packet_loop_options_t *>(callback_arg);
    if (options) {
      options->do_time_check = 1;
    }
    break;
  }

  case picoquic_packet_loop_time_check: {
    auto *timeArg = static_cast<packet_loop_time_check_arg_t *>(callback_arg);
    if (!timeArg) {
      break;
    }

    bool hasPending = hasPendingWork();
    // If we have pending tasks, wake up immediately
    if (hasPending) {
      XLOG(DBG6) << "PicoQuicExecutor: hasPendingWork=true, "
                 << "setting delta_t=0";
      timeArg->delta_t = 0;
    } else {
      // Otherwise, wake up for the next timer
      int64_t timerDelta = getNextTimeoutDelta(timeArg->current_time);
      timeArg->delta_t = std::min(timeArg->delta_t, timerDelta);
      // Cap at 200ms to ensure we drain tasks queued from other threads
      constexpr int64_t kMaxLoopSleepUs = 200000; // 200ms in microseconds
      timeArg->delta_t = std::min(timeArg->delta_t, kMaxLoopSleepUs);
    }
    break;
  }

  case picoquic_packet_loop_after_receive: {
    // Process all pending work after receiving packets
    XLOG(DBG6) << "PicoQuicExecutor: after_receive";
    uint64_t currentTime = picoquic_current_time();
    drainTasks();
    processExpiredTimers(currentTime);
    break;
  }

  case picoquic_packet_loop_after_send: {
    // Also drain tasks after sending in case work was queued
    XLOG(DBG6) << "PicoQuicExecutor: after_send";
    uint64_t currentTime = picoquic_current_time();
    drainTasks();
    processExpiredTimers(currentTime);
    break;
  }

  default:
    break;
  }

  return 0;
}

} // namespace moxygen
