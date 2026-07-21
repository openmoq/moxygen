/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/logging/xlog.h>
#include <algorithm>
#include <chrono>
#include <functional>
#include <optional>

namespace moxygen {

/**
 * Manages delivery timeout from multiple sources and computes the effective
 * timeout as min(publisher, subscriber).
 *
 * When the effective timeout changes, automatically invokes a registered
 * callback to notify the owner.
 */
class MoQDeliveryTimeoutManager {
 public:
  // Callback type invoked when effective timeout changes
  using OnChangeCallback =
      std::function<void(std::optional<std::chrono::milliseconds>)>;

  MoQDeliveryTimeoutManager() = default;

  // Set callback to be invoked when effective timeout changes
  void setOnChangeCallback(OnChangeCallback callback) {
    onChangeCallback_ = std::move(callback);
  }

  // Set publisher timeout (automatically triggers callback if effective
  // changes)
  void setPublisherTimeout(std::chrono::milliseconds timeout) {
    publisherTimeout_ = toTimeoutSource(timeout);
    notifyIfChanged();
  }

  // Set subscriber timeout (automatically triggers callback if effective
  // changes)
  void setSubscriberTimeout(std::chrono::milliseconds timeout) {
    XLOG(DBG6)
        << "MoQDeliveryTimeoutManager::setSubscriberTimeout: SETTING subscriber timeout"
        << " timeout=" << timeout.count() << "ms"
        << " previousSubscriber="
        << (subscriberTimeout_.has_value()
                ? std::to_string(subscriberTimeout_->count()) + "ms"
                : "none");
    subscriberTimeout_ = toTimeoutSource(timeout);
    notifyIfChanged();
  }

  // Get effective timeout as min of available sources
  std::optional<std::chrono::milliseconds> getEffectiveTimeout() const {
    if (publisherTimeout_ && subscriberTimeout_) {
      return std::min(*publisherTimeout_, *subscriberTimeout_);
    }
    if (publisherTimeout_) {
      return publisherTimeout_;
    }
    if (subscriberTimeout_) {
      return subscriberTimeout_;
    }
    return std::nullopt;
  }

 private:
  // A timeout of 0 means "no timeout" (draft 18+): such a source imposes no
  // constraint and is dropped from the min() computation. A non-zero value is
  // kept as-is.
  static std::optional<std::chrono::milliseconds> toTimeoutSource(
      std::chrono::milliseconds timeout) {
    if (timeout.count() == 0) {
      return std::nullopt;
    }
    return timeout;
  }

  // Individual timeout sources
  std::optional<std::chrono::milliseconds> publisherTimeout_;
  std::optional<std::chrono::milliseconds> subscriberTimeout_;

  // Cached effective timeout for change detection
  mutable std::optional<std::chrono::milliseconds> lastEffectiveTimeout_;

  // Callback invoked when effective timeout changes
  OnChangeCallback onChangeCallback_;

  // Check if effective timeout changed and notify callback
  void notifyIfChanged() {
    auto current = getEffectiveTimeout();
    if (current != lastEffectiveTimeout_) {
      XLOG(DBG6)
          << "MoQDeliveryTimeoutManager::notifyIfChanged: EFFECTIVE TIMEOUT CHANGED"
          << " previous="
          << (lastEffectiveTimeout_.has_value()
                  ? std::to_string(lastEffectiveTimeout_->count()) + "ms"
                  : "none")
          << " current="
          << (current.has_value() ? std::to_string(current->count()) + "ms"
                                  : "none")
          << " - INVOKING CALLBACK";
      lastEffectiveTimeout_ = current;
      if (onChangeCallback_) {
        onChangeCallback_(current);
      }
    } else {
      XLOG(DBG6)
          << "MoQDeliveryTimeoutManager::notifyIfChanged: Effective timeout unchanged"
          << " current="
          << (current.has_value() ? std::to_string(current->count()) + "ms"
                                  : "none")
          << " - NO CALLBACK";
    }
  }
};

} // namespace moxygen
