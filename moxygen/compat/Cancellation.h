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
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace folly {

// =============================================================================
// CancellationToken/Source - Thread-safe cancellation signaling
// =============================================================================
// Fixed implementation that properly unregisters callbacks to prevent:
// - Memory leaks (callbacks stored forever)
// - Use-after-free (calling into destroyed callback objects)
//
// Uses unique IDs instead of storing raw pointers/lambdas capturing 'this'.
// =============================================================================

class CancellationSource;
class CancellationCallback;

class CancellationToken {
 public:
  CancellationToken() = default;

  // Check if cancellation has been requested
  bool isCancellationRequested() const noexcept {
    return state_ && state_->cancelled.load(std::memory_order_acquire);
  }

  // Check if the token can be cancelled
  bool canBeCancelled() const noexcept {
    return state_ != nullptr;
  }

 private:
  friend class CancellationSource;
  friend class CancellationCallback;

  struct CallbackEntry {
    std::function<void()> callback;
    bool active{true};  // Set to false when unregistered
  };

  struct State {
    std::atomic<bool> cancelled{false};
    std::atomic<uint64_t> nextCallbackId{1};
    std::mutex mutex;
    std::unordered_map<uint64_t, CallbackEntry> callbacks;
    bool invoking{false};  // True while invoking callbacks
  };

  explicit CancellationToken(std::shared_ptr<State> state)
      : state_(std::move(state)) {}

  std::shared_ptr<State> state_;
};

class CancellationSource {
 public:
  CancellationSource() : state_(std::make_shared<CancellationToken::State>()) {}

  // Default copy/move
  CancellationSource(const CancellationSource&) = default;
  CancellationSource(CancellationSource&&) = default;
  CancellationSource& operator=(const CancellationSource&) = default;
  CancellationSource& operator=(CancellationSource&&) = default;

  // Get a token that can be used to check for cancellation
  CancellationToken getToken() const {
    return CancellationToken(state_);
  }

  // Request cancellation - returns true if this call triggered cancellation
  bool requestCancellation() {
    if (!state_) {
      return false;
    }

    bool expected = false;
    if (!state_->cancelled.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
      return false; // Already cancelled
    }

    // Collect and invoke callbacks
    std::vector<std::function<void()>> toInvoke;
    {
      std::lock_guard<std::mutex> lock(state_->mutex);
      state_->invoking = true;
      for (auto& [id, entry] : state_->callbacks) {
        if (entry.active && entry.callback) {
          toInvoke.push_back(std::move(entry.callback));
        }
      }
      state_->callbacks.clear();
    }

    // Invoke outside lock to prevent deadlocks
    for (auto& cb : toInvoke) {
      try {
        cb();
      } catch (...) {
        // Swallow exceptions from callbacks
      }
    }

    {
      std::lock_guard<std::mutex> lock(state_->mutex);
      state_->invoking = false;
    }

    return true;
  }

  // Check if cancellation was requested
  bool isCancellationRequested() const noexcept {
    return state_ && state_->cancelled.load(std::memory_order_acquire);
  }

 private:
  friend class CancellationCallback;
  std::shared_ptr<CancellationToken::State> state_;
};

// =============================================================================
// CancellationCallback - RAII callback that fires on cancellation
// =============================================================================
// Properly unregisters on destruction to prevent use-after-free.

class CancellationCallback {
 public:
  // Register callback - invokes immediately if already cancelled
  template <typename F>
  CancellationCallback(const CancellationToken& token, F&& callback)
      : state_(token.state_) {
    if (!state_) {
      return;
    }

    // If already cancelled, invoke immediately and don't register
    if (state_->cancelled.load(std::memory_order_acquire)) {
      try {
        callback();
      } catch (...) {
        // Swallow exceptions
      }
      state_ = nullptr;  // Don't need to unregister
      return;
    }

    // Register callback with unique ID
    {
      std::lock_guard<std::mutex> lock(state_->mutex);

      // Double-check after acquiring lock
      if (state_->cancelled.load(std::memory_order_acquire)) {
        // Will invoke below, outside lock
      } else {
        callbackId_ = state_->nextCallbackId.fetch_add(1, std::memory_order_relaxed);
        state_->callbacks[callbackId_] = {std::forward<F>(callback), true};
        return;  // Successfully registered
      }
    }

    // Cancelled while we were registering - invoke now
    try {
      callback();
    } catch (...) {
      // Swallow exceptions
    }
    state_ = nullptr;  // Don't need to unregister
  }

  ~CancellationCallback() {
    unregister();
  }

  // Move-only
  CancellationCallback(CancellationCallback&& other) noexcept
      : state_(std::move(other.state_)), callbackId_(other.callbackId_) {
    other.callbackId_ = 0;
  }

  CancellationCallback& operator=(CancellationCallback&& other) noexcept {
    if (this != &other) {
      unregister();
      state_ = std::move(other.state_);
      callbackId_ = other.callbackId_;
      other.callbackId_ = 0;
    }
    return *this;
  }

  CancellationCallback(const CancellationCallback&) = delete;
  CancellationCallback& operator=(const CancellationCallback&) = delete;

  // Manually unregister (also called by destructor)
  void unregister() noexcept {
    if (!state_ || callbackId_ == 0) {
      return;
    }

    std::lock_guard<std::mutex> lock(state_->mutex);
    auto it = state_->callbacks.find(callbackId_);
    if (it != state_->callbacks.end()) {
      it->second.active = false;  // Mark inactive
      // Only erase if not currently invoking (to avoid iterator invalidation)
      if (!state_->invoking) {
        state_->callbacks.erase(it);
      }
    }
    callbackId_ = 0;
  }

 private:
  std::shared_ptr<CancellationToken::State> state_;
  uint64_t callbackId_{0};
};

} // namespace folly

#endif // !MOXYGEN_USE_FOLLY

// Bring into compat namespace
namespace moxygen::compat {
using CancellationToken = folly::CancellationToken;
using CancellationSource = folly::CancellationSource;
using CancellationCallback = folly::CancellationCallback;
} // namespace moxygen::compat
