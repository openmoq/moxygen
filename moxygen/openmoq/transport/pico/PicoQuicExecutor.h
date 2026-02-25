/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/Function.h>
#include <functional>
#include <memory>
#include <moxygen/events/MoQExecutor.h>
#include <mutex>
#include <picoquic.h>
#include <picoquic_packet_loop.h>
#include <queue>
#include <quic/common/events/QuicEventBase.h>
#include <vector>

namespace moxygen {

/**
 * PicoQuicExecutor - Executor implementation for picoquic event loop
 *
 * This executor integrates with picoquic's packet loop callbacks to:
 * - Execute tasks queued via add()
 * - Fire timers scheduled via scheduleTimeout()
 * - Set epoll timeout to 0 when work is pending
 *
 * Usage:
 *   auto executor = std::make_shared<PicoQuicExecutor>();
 *   // Pass executor->getLoopCallback() to picoquic_packet_loop
 *   // Pass executor.get() as loop_callback_ctx
 */
class PicoQuicExecutor : public MoQExecutor {
public:
  PicoQuicExecutor();
  ~PicoQuicExecutor() override = default;

  // folly::Executor interface
  void add(folly::Func f) override;

  // MoQExecutor interface
  void scheduleTimeout(quic::QuicTimerCallback *callback,
                       std::chrono::milliseconds timeout) override;

  // Get the loop callback function to pass to picoquic_packet_loop
  static picoquic_packet_loop_cb_fn getLoopCallback() {
    return &loopCallbackStatic;
  }

  // Process work - called from the packet loop callback
  void drainTasks();
  void processExpiredTimers(uint64_t currentTime);
  bool hasPendingWork() const;
  int64_t getNextTimeoutDelta(uint64_t currentTime) const;

private:
  struct TimerEntry {
    uint64_t expiryTime;
    quic::QuicTimerCallback *callback;
    bool cancelled{false};

    bool operator>(const TimerEntry &other) const {
      return expiryTime > other.expiryTime;
    }
  };

  // Implementation of QuicTimerCallback::TimerCallbackImpl for cancellation
  class TimerCallbackImplHandle
      : public quic::QuicTimerCallback::TimerCallbackImpl {
  public:
    explicit TimerCallbackImplHandle(std::shared_ptr<TimerEntry> entry)
        : entry_(std::move(entry)) {}

    void cancelImpl() noexcept override {
      if (auto entry = entry_.lock()) {
        entry->cancelled = true;
      }
    }

    bool isScheduledImpl() const noexcept override {
      if (auto entry = entry_.lock()) {
        return !entry->cancelled;
      }
      return false;
    }

    std::chrono::milliseconds getTimeRemainingImpl() const noexcept override {
      auto entry = entry_.lock();
      if (!entry || entry->cancelled) {
        return std::chrono::milliseconds(0);
      }

      uint64_t currentTime = picoquic_current_time();
      if (entry->expiryTime <= currentTime) {
        return std::chrono::milliseconds(0);
      }

      // picoquic times are in microseconds
      uint64_t remainingUs = entry->expiryTime - currentTime;
      return std::chrono::milliseconds(remainingUs / 1000);
    }

  private:
    std::weak_ptr<TimerEntry> entry_;
  };

  static int loopCallbackStatic(picoquic_quic_t *quic,
                                picoquic_packet_loop_cb_enum cb_mode,
                                void *callback_ctx, void *callback_arg);

  int loopCallback(picoquic_quic_t *quic, picoquic_packet_loop_cb_enum cb_mode,
                   void *callback_arg);

  mutable std::mutex taskMutex_;
  std::queue<folly::Func> tasks_;

  mutable std::mutex timerMutex_;
  std::priority_queue<std::shared_ptr<TimerEntry>,
                      std::vector<std::shared_ptr<TimerEntry>>,
                      std::function<bool(const std::shared_ptr<TimerEntry> &,
                                         const std::shared_ptr<TimerEntry> &)>>
      timers_; // min-heap by expiry time
};

} // namespace moxygen
