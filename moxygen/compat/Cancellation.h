/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <moxygen/compat/Config.h>

#if MOXYGEN_USE_FOLLY
#include <folly/CancellationToken.h>
#else

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace folly {

// Minimal std-mode replacement for folly::CancellationToken
// This provides basic cancellation signaling without the full folly
// implementation

class CancellationSource;

class CancellationToken {
 public:
  CancellationToken() = default;

  // Check if cancellation has been requested
  bool isCancellationRequested() const {
    return state_ && state_->cancelled.load(std::memory_order_acquire);
  }

  // Check if the token can be cancelled
  bool canBeCancelled() const {
    return state_ != nullptr;
  }

 private:
  friend class CancellationSource;
  friend class CancellationCallback;

  struct State {
    std::atomic<bool> cancelled{false};
    std::mutex mutex;
    std::vector<std::function<void()>> callbacks;
  };

  explicit CancellationToken(std::shared_ptr<State> state)
      : state_(std::move(state)) {}

  std::shared_ptr<State> state_;
};

class CancellationSource {
 public:
  CancellationSource() : state_(std::make_shared<CancellationToken::State>()) {}

  // Get a token that can be used to check for cancellation
  CancellationToken getToken() const {
    return CancellationToken(state_);
  }

  // Request cancellation
  bool requestCancellation() {
    if (!state_) {
      return false;
    }

    bool expected = false;
    if (!state_->cancelled.compare_exchange_strong(
            expected, true, std::memory_order_release)) {
      return false; // Already cancelled
    }

    // Invoke callbacks
    std::vector<std::function<void()>> callbacks;
    {
      std::lock_guard<std::mutex> lock(state_->mutex);
      callbacks = std::move(state_->callbacks);
    }
    for (auto& cb : callbacks) {
      cb();
    }
    return true;
  }

 private:
  friend class CancellationCallback;
  std::shared_ptr<CancellationToken::State> state_;
};

// RAII callback that fires when cancellation is requested
class CancellationCallback {
 public:
  template <typename F>
  CancellationCallback(const CancellationToken& token, F&& callback)
      : state_(token.state_) {
    if (!state_) {
      return;
    }

    // If already cancelled, invoke immediately
    if (state_->cancelled.load(std::memory_order_acquire)) {
      callback();
      return;
    }

    // Register callback
    {
      std::lock_guard<std::mutex> lock(state_->mutex);
      // Double-check after acquiring lock
      if (state_->cancelled.load(std::memory_order_acquire)) {
        // Invoke outside lock
      } else {
        callback_ = std::forward<F>(callback);
        state_->callbacks.push_back([this]() {
          if (callback_) {
            callback_();
          }
        });
        registered_ = true;
        return;
      }
    }
    // Cancelled while we were registering
    callback();
  }

  ~CancellationCallback() {
    // Note: For simplicity, we don't unregister.
    // In a full implementation, we would remove from callbacks list.
    callback_ = nullptr;
  }

  CancellationCallback(const CancellationCallback&) = delete;
  CancellationCallback& operator=(const CancellationCallback&) = delete;

 private:
  std::shared_ptr<CancellationToken::State> state_;
  std::function<void()> callback_;
  bool registered_{false};
};

} // namespace folly

#endif // !MOXYGEN_USE_FOLLY
