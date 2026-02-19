/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <moxygen/compat/Config.h>
#include <moxygen/compat/Unit.h>

#if MOXYGEN_USE_FOLLY
#include <folly/coro/Task.h>
#include <folly/futures/Future.h>
#else
// Include std headers outside namespace
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <variant>
#endif

namespace moxygen::compat {

#if MOXYGEN_USE_FOLLY

template <typename T>
using Task = folly::coro::Task<T>;

template <typename T>
using SemiFuture = folly::SemiFuture<T>;

template <typename T>
using Future = folly::Future<T>;

template <typename T>
using Promise = folly::Promise<T>;

template <typename T>
SemiFuture<T> makeSemiFuture(T&& val) {
  return folly::makeSemiFuture(std::forward<T>(val));
}

inline SemiFuture<Unit> makeSemiFuture() {
  return folly::makeSemiFuture(folly::unit);
}

#else

// =============================================================================
// SemiFuture/Promise - Thread-safe async primitives for std mode
// =============================================================================

// Forward declarations
template <typename T>
class SemiFuture;

template <typename T>
class Promise;

namespace detail {

// Shared state between Promise and SemiFuture
template <typename T>
struct FutureState {
  std::mutex mutex;
  std::condition_variable cv;
  std::variant<std::monostate, T, std::exception_ptr> result;
  std::atomic<bool> ready{false};
  std::atomic<bool> consumed{false};
  std::function<void(T)> continuation;
  std::function<void(std::exception_ptr)> errorHandler;

  bool isReady() const noexcept {
    return ready.load(std::memory_order_acquire);
  }

  bool hasValue() const noexcept {
    return std::holds_alternative<T>(result);
  }

  bool hasException() const noexcept {
    return std::holds_alternative<std::exception_ptr>(result);
  }
};

// Specialization for void
template <>
struct FutureState<void> {
  std::mutex mutex;
  std::condition_variable cv;
  std::exception_ptr exception;
  std::atomic<bool> ready{false};
  std::atomic<bool> consumed{false};
  std::atomic<bool> hasException_{false};
  std::function<void()> continuation;
  std::function<void(std::exception_ptr)> errorHandler;

  bool isReady() const noexcept {
    return ready.load(std::memory_order_acquire);
  }

  bool hasException() const noexcept {
    return hasException_.load(std::memory_order_acquire);
  }
};

} // namespace detail

// =============================================================================
// SemiFuture<T> - Represents a value that will be available in the future
// =============================================================================

template <typename T>
class SemiFuture {
 public:
  using value_type = T;

  SemiFuture() = default;

  // Construct with immediate value
  explicit SemiFuture(T value) : state_(std::make_shared<detail::FutureState<T>>()) {
    state_->result = std::move(value);
    state_->ready.store(true, std::memory_order_release);
  }

  // Construct with exception
  explicit SemiFuture(std::exception_ptr ex) : state_(std::make_shared<detail::FutureState<T>>()) {
    state_->result = std::move(ex);
    state_->ready.store(true, std::memory_order_release);
  }

  // Check if result is ready
  bool isReady() const noexcept {
    return state_ && state_->isReady();
  }

  // Check if valid (has shared state)
  bool valid() const noexcept {
    return state_ != nullptr;
  }

  // Get value (blocks if not ready, throws if exception)
  T value() {
    if (!state_) {
      throw std::runtime_error("SemiFuture has no state");
    }

    // Wait for result
    wait();

    // Check single-consumption
    bool expected = false;
    if (!state_->consumed.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
      throw std::runtime_error("SemiFuture value already consumed");
    }

    // Return or throw
    if (state_->hasException()) {
      std::rethrow_exception(std::get<std::exception_ptr>(state_->result));
    }
    return std::move(std::get<T>(state_->result));
  }

  // Get value with timeout
  template <typename Rep, typename Period>
  std::optional<T> value(std::chrono::duration<Rep, Period> timeout) {
    if (!state_) {
      throw std::runtime_error("SemiFuture has no state");
    }

    if (!wait_for(timeout)) {
      return std::nullopt;
    }

    bool expected = false;
    if (!state_->consumed.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
      throw std::runtime_error("SemiFuture value already consumed");
    }

    if (state_->hasException()) {
      std::rethrow_exception(std::get<std::exception_ptr>(state_->result));
    }
    return std::move(std::get<T>(state_->result));
  }

  // Wait for result (blocking)
  void wait() const {
    if (!state_) return;
    if (state_->isReady()) return;

    std::unique_lock<std::mutex> lock(state_->mutex);
    state_->cv.wait(lock, [this] { return state_->isReady(); });
  }

  // Wait with timeout - returns true if ready
  template <typename Rep, typename Period>
  bool wait_for(std::chrono::duration<Rep, Period> timeout) const {
    if (!state_) return false;
    if (state_->isReady()) return true;

    std::unique_lock<std::mutex> lock(state_->mutex);
    return state_->cv.wait_for(lock, timeout, [this] { return state_->isReady(); });
  }

  // Add continuation - returns new future with transformed result
  template <typename F>
  auto thenValue(F&& f) -> SemiFuture<std::invoke_result_t<F, T>> {
    using U = std::invoke_result_t<F, T>;
    auto promise = std::make_shared<Promise<U>>();
    auto future = promise->getSemiFuture();

    if (!state_) {
      promise->setException(std::make_exception_ptr(
          std::runtime_error("SemiFuture has no state")));
      return future;
    }

    auto continuation = [promise, f = std::forward<F>(f)](T val) mutable {
      try {
        if constexpr (std::is_void_v<U>) {
          f(std::move(val));
          promise->setValue();
        } else {
          promise->setValue(f(std::move(val)));
        }
      } catch (...) {
        promise->setException(std::current_exception());
      }
    };

    auto errorHandler = [promise](std::exception_ptr ex) {
      promise->setException(std::move(ex));
    };

    {
      std::lock_guard<std::mutex> lock(state_->mutex);
      if (state_->isReady()) {
        // Already ready - invoke immediately (outside lock)
        if (state_->hasException()) {
          errorHandler(std::get<std::exception_ptr>(state_->result));
        } else {
          continuation(std::move(std::get<T>(state_->result)));
        }
      } else {
        // Store continuation
        state_->continuation = std::move(continuation);
        state_->errorHandler = std::move(errorHandler);
      }
    }

    return future;
  }

 private:
  friend class Promise<T>;

  explicit SemiFuture(std::shared_ptr<detail::FutureState<T>> state)
      : state_(std::move(state)) {}

  std::shared_ptr<detail::FutureState<T>> state_;
};

// =============================================================================
// SemiFuture<void> specialization
// =============================================================================

template <>
class SemiFuture<void> {
 public:
  using value_type = void;

  SemiFuture() = default;

  // Construct as completed
  explicit SemiFuture(Unit) : state_(std::make_shared<detail::FutureState<void>>()) {
    state_->ready.store(true, std::memory_order_release);
  }

  // Construct with exception
  explicit SemiFuture(std::exception_ptr ex)
      : state_(std::make_shared<detail::FutureState<void>>()) {
    state_->exception = std::move(ex);
    state_->hasException_.store(true, std::memory_order_release);
    state_->ready.store(true, std::memory_order_release);
  }

  bool isReady() const noexcept {
    return state_ && state_->isReady();
  }

  bool valid() const noexcept {
    return state_ != nullptr;
  }

  void value() {
    if (!state_) {
      throw std::runtime_error("SemiFuture has no state");
    }
    wait();

    bool expected = false;
    if (!state_->consumed.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
      throw std::runtime_error("SemiFuture value already consumed");
    }

    if (state_->hasException()) {
      std::rethrow_exception(state_->exception);
    }
  }

  void wait() const {
    if (!state_) return;
    if (state_->isReady()) return;

    std::unique_lock<std::mutex> lock(state_->mutex);
    state_->cv.wait(lock, [this] { return state_->isReady(); });
  }

  template <typename Rep, typename Period>
  bool wait_for(std::chrono::duration<Rep, Period> timeout) const {
    if (!state_) return false;
    if (state_->isReady()) return true;

    std::unique_lock<std::mutex> lock(state_->mutex);
    return state_->cv.wait_for(lock, timeout, [this] { return state_->isReady(); });
  }

  template <typename F>
  auto thenValue(F&& f) -> SemiFuture<std::invoke_result_t<F>> {
    using U = std::invoke_result_t<F>;
    auto promise = std::make_shared<Promise<U>>();
    auto future = promise->getSemiFuture();

    if (!state_) {
      promise->setException(std::make_exception_ptr(
          std::runtime_error("SemiFuture has no state")));
      return future;
    }

    auto continuation = [promise, f = std::forward<F>(f)]() mutable {
      try {
        if constexpr (std::is_void_v<U>) {
          f();
          promise->setValue();
        } else {
          promise->setValue(f());
        }
      } catch (...) {
        promise->setException(std::current_exception());
      }
    };

    auto errorHandler = [promise](std::exception_ptr ex) {
      promise->setException(std::move(ex));
    };

    {
      std::lock_guard<std::mutex> lock(state_->mutex);
      if (state_->isReady()) {
        if (state_->hasException()) {
          errorHandler(state_->exception);
        } else {
          continuation();
        }
      } else {
        state_->continuation = std::move(continuation);
        state_->errorHandler = std::move(errorHandler);
      }
    }

    return future;
  }

 private:
  friend class Promise<void>;

  explicit SemiFuture(std::shared_ptr<detail::FutureState<void>> state)
      : state_(std::move(state)) {}

  std::shared_ptr<detail::FutureState<void>> state_;
};

// =============================================================================
// Promise<T> - Produces values for SemiFuture
// =============================================================================

template <typename T>
class Promise {
 public:
  Promise() : state_(std::make_shared<detail::FutureState<T>>()) {}

  // Move-only
  Promise(Promise&&) = default;
  Promise& operator=(Promise&&) = default;
  Promise(const Promise&) = delete;
  Promise& operator=(const Promise&) = delete;

  // Get the future associated with this promise
  SemiFuture<T> getSemiFuture() {
    if (futureRetrieved_) {
      throw std::runtime_error("SemiFuture already retrieved");
    }
    futureRetrieved_ = true;
    return SemiFuture<T>(state_);
  }

  // Set the value
  void setValue(T value) {
    std::function<void(T)> continuation;
    {
      std::lock_guard<std::mutex> lock(state_->mutex);
      if (state_->isReady()) {
        throw std::runtime_error("Promise already fulfilled");
      }
      state_->result = std::move(value);
      state_->ready.store(true, std::memory_order_release);
      continuation = std::move(state_->continuation);
    }
    state_->cv.notify_all();

    if (continuation) {
      try {
        continuation(std::move(std::get<T>(state_->result)));
      } catch (...) {
        // Swallow continuation exceptions
      }
    }
  }

  // Set exception
  void setException(std::exception_ptr ex) {
    std::function<void(std::exception_ptr)> errorHandler;
    {
      std::lock_guard<std::mutex> lock(state_->mutex);
      if (state_->isReady()) {
        throw std::runtime_error("Promise already fulfilled");
      }
      state_->result = std::move(ex);
      state_->ready.store(true, std::memory_order_release);
      errorHandler = std::move(state_->errorHandler);
    }
    state_->cv.notify_all();

    if (errorHandler) {
      try {
        errorHandler(std::get<std::exception_ptr>(state_->result));
      } catch (...) {
        // Swallow handler exceptions
      }
    }
  }

  // Check if fulfilled
  bool isFulfilled() const noexcept {
    return state_->isReady();
  }

 private:
  std::shared_ptr<detail::FutureState<T>> state_;
  bool futureRetrieved_{false};
};

// =============================================================================
// Promise<void> specialization
// =============================================================================

template <>
class Promise<void> {
 public:
  Promise() : state_(std::make_shared<detail::FutureState<void>>()) {}

  Promise(Promise&&) = default;
  Promise& operator=(Promise&&) = default;
  Promise(const Promise&) = delete;
  Promise& operator=(const Promise&) = delete;

  SemiFuture<void> getSemiFuture() {
    if (futureRetrieved_) {
      throw std::runtime_error("SemiFuture already retrieved");
    }
    futureRetrieved_ = true;
    return SemiFuture<void>(state_);
  }

  void setValue() {
    std::function<void()> continuation;
    {
      std::lock_guard<std::mutex> lock(state_->mutex);
      if (state_->isReady()) {
        throw std::runtime_error("Promise already fulfilled");
      }
      state_->ready.store(true, std::memory_order_release);
      continuation = std::move(state_->continuation);
    }
    state_->cv.notify_all();

    if (continuation) {
      try {
        continuation();
      } catch (...) {
      }
    }
  }

  // Convenience for Unit
  void setValue(Unit) {
    setValue();
  }

  void setException(std::exception_ptr ex) {
    std::function<void(std::exception_ptr)> errorHandler;
    {
      std::lock_guard<std::mutex> lock(state_->mutex);
      if (state_->isReady()) {
        throw std::runtime_error("Promise already fulfilled");
      }
      state_->exception = std::move(ex);
      state_->hasException_.store(true, std::memory_order_release);
      state_->ready.store(true, std::memory_order_release);
      errorHandler = std::move(state_->errorHandler);
    }
    state_->cv.notify_all();

    if (errorHandler) {
      try {
        errorHandler(state_->exception);
      } catch (...) {
      }
    }
  }

  bool isFulfilled() const noexcept {
    return state_->isReady();
  }

 private:
  std::shared_ptr<detail::FutureState<void>> state_;
  bool futureRetrieved_{false};
};

// =============================================================================
// Factory functions
// =============================================================================

template <typename T>
SemiFuture<T> makeSemiFuture(T&& val) {
  return SemiFuture<T>(std::forward<T>(val));
}

inline SemiFuture<Unit> makeSemiFuture() {
  return SemiFuture<Unit>(unit);
}

inline SemiFuture<void> makeSemiFutureVoid() {
  return SemiFuture<void>(unit);
}

// Create a future that is immediately ready with an exception
template <typename T>
SemiFuture<T> makeSemiFutureException(std::exception_ptr ex) {
  return SemiFuture<T>(std::move(ex));
}

template <typename T, typename E>
SemiFuture<T> makeSemiFutureException(E&& e) {
  return SemiFuture<T>(std::make_exception_ptr(std::forward<E>(e)));
}

#endif

} // namespace moxygen::compat
