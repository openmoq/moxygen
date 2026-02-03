/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <moxygen/compat/Config.h>
#include <moxygen/compat/Containers.h>
#include <moxygen/compat/Debug.h>
#include <moxygen/MoQFramer.h>
#include <moxygen/events/MoQExecutor.h>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>

#if MOXYGEN_USE_FOLLY
#include <quic/common/events/QuicTimer.h>
#endif

// Forward declarations
namespace moxygen {

class ObjectTimerCallback;

// Callback type for session close functionality
using StreamResetCallback = std::function<void(ResetStreamErrorCode)>;

/*
 * Stream scoped delivery timeout manager used to track per object delivery
 * timeout.
 */
class MoQDeliveryTimer {
 public:
  MoQDeliveryTimer(
      std::shared_ptr<MoQExecutor> exec,
      std::chrono::milliseconds deliveryTimeout,
      StreamResetCallback streamResetCallback)
      : exec_(std::move(exec)),
        streamResetCallback_(std::move(streamResetCallback)),
        deliveryTimeout_(deliveryTimeout) {}
  ~MoQDeliveryTimer();

  /*
   * Sets the delivery timeout.
   */
  void setDeliveryTimeout(std::chrono::milliseconds timeout);

  /**
   * Start the delivery timeout timer
   */
  void startTimer(uint64_t objectId, std::chrono::microseconds srtt);

  /**
   * Cancel the active delivery timeout timer for a specific object
   */
  void cancelTimer(uint64_t objectId);

  /**
   * Cancel all active delivery timeout timers for a specific stream
   */
  void cancelAllTimers();

 private:
  friend class ObjectTimerCallback;

  /*
   * Calculates the effective timeout that will be used for object timers
   */
  std::chrono::milliseconds calculateTimeout(std::chrono::microseconds srtt);

  std::shared_ptr<MoQExecutor> exec_;
  StreamResetCallback streamResetCallback_;
  // (objectId -> timer)
  compat::FastMap<uint64_t, std::unique_ptr<ObjectTimerCallback>>
      objectTimers_;
  std::chrono::milliseconds deliveryTimeout_;
};

#if MOXYGEN_USE_FOLLY

class ObjectTimerCallback : public quic::QuicTimerCallback {
 public:
  ObjectTimerCallback(uint64_t objectId, MoQDeliveryTimer& owner)
      : objectId_(objectId), owner_(owner) {}

  void timeoutExpired() noexcept override;

  // Don't invoke callback when timer is cancelled
  void callbackCanceled() noexcept override {}

 private:
  uint64_t objectId_;
  MoQDeliveryTimer& owner_;
};

#else // !MOXYGEN_USE_FOLLY

// Std-mode: stub timer callback
class ObjectTimerCallback {
 public:
  ObjectTimerCallback(uint64_t objectId, MoQDeliveryTimer& owner)
      : objectId_(objectId), owner_(owner) {}

  void timeoutExpired() noexcept;
  void callbackCanceled() noexcept {}

 private:
  uint64_t objectId_;
  MoQDeliveryTimer& owner_;
};

#endif // MOXYGEN_USE_FOLLY

} // namespace moxygen
