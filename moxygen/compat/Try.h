/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <moxygen/compat/Config.h>

#if MOXYGEN_USE_FOLLY
#include <folly/Try.h>
#else

#include <exception>
#include <functional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

namespace folly {

// =============================================================================
// Try<T> - Enhanced error handling with exception capture
// =============================================================================
// Holds either a value, an exception, or nothing (empty state).
// Requires: C++17 or later (uses std::variant, std::invoke, if constexpr)
//
// Features:
// - Exception-safe value access
// - Monadic chaining: thenValue(), thenTry()
// - Exception introspection: hasException<E>(), tryGetExceptionObject<E>()
// - Factory function: makeTryWith() for exception capture
// =============================================================================

template <typename T>
class Try {
 public:
  using value_type = T;

  // Default constructor - empty state
  Try() = default;

  // Construct with value
  /* implicit */ Try(T value) : storage_(std::move(value)) {}

  // Construct with exception
  /* implicit */ Try(std::exception_ptr ex) : storage_(std::move(ex)) {}

  // Copy/move (defaulted)
  Try(const Try&) = default;
  Try(Try&&) = default;
  Try& operator=(const Try&) = default;
  Try& operator=(Try&&) = default;

  // ==========================================================================
  // State observers
  // ==========================================================================

  bool hasValue() const noexcept {
    return std::holds_alternative<T>(storage_);
  }

  bool hasException() const noexcept {
    return std::holds_alternative<std::exception_ptr>(storage_);
  }

  bool isEmpty() const noexcept {
    return std::holds_alternative<std::monostate>(storage_);
  }

  explicit operator bool() const noexcept {
    return hasValue();
  }

  // ==========================================================================
  // Value access (throws on exception/empty)
  // ==========================================================================

  T& value() & {
    throwIfFailed();
    return std::get<T>(storage_);
  }

  const T& value() const& {
    throwIfFailed();
    return std::get<T>(storage_);
  }

  T&& value() && {
    throwIfFailed();
    return std::get<T>(std::move(storage_));
  }

  // Unchecked access
  T& operator*() & { return value(); }
  const T& operator*() const& { return value(); }
  T&& operator*() && { return std::move(*this).value(); }

  T* operator->() { return &value(); }
  const T* operator->() const { return &value(); }

  // ==========================================================================
  // Exception access
  // ==========================================================================

  std::exception_ptr exception() const noexcept {
    if (hasException()) {
      return std::get<std::exception_ptr>(storage_);
    }
    return nullptr;
  }

  // Check if exception is of specific type
  template <typename E>
  bool hasExceptionOf() const noexcept {
    return tryGetExceptionObject<E>() != nullptr;
  }

  // Get exception object if it matches type E
  template <typename E>
  const E* tryGetExceptionObject() const noexcept {
    if (!hasException()) {
      return nullptr;
    }
    try {
      std::rethrow_exception(std::get<std::exception_ptr>(storage_));
    } catch (const E& e) {
      return &e;
    } catch (...) {
      return nullptr;
    }
  }

  // Get exception message (if std::exception derived)
  std::string exceptionMessage() const {
    if (auto* e = tryGetExceptionObject<std::exception>()) {
      return e->what();
    }
    return hasException() ? "unknown exception" : "";
  }

  // ==========================================================================
  // Monadic operations
  // ==========================================================================

  // thenValue: Apply function to value, wrap result in Try
  // F: T -> U
  // Returns: Try<U>
  template <typename F>
  auto thenValue(F&& f) & -> Try<std::invoke_result_t<F, T&>> {
    using U = std::invoke_result_t<F, T&>;
    if (hasValue()) {
      try {
        return Try<U>(std::invoke(std::forward<F>(f), value()));
      } catch (...) {
        return Try<U>(std::current_exception());
      }
    }
    if (hasException()) {
      return Try<U>(exception());
    }
    return Try<U>();  // Empty
  }

  template <typename F>
  auto thenValue(F&& f) const& -> Try<std::invoke_result_t<F, const T&>> {
    using U = std::invoke_result_t<F, const T&>;
    if (hasValue()) {
      try {
        return Try<U>(std::invoke(std::forward<F>(f), value()));
      } catch (...) {
        return Try<U>(std::current_exception());
      }
    }
    if (hasException()) {
      return Try<U>(exception());
    }
    return Try<U>();
  }

  template <typename F>
  auto thenValue(F&& f) && -> Try<std::invoke_result_t<F, T&&>> {
    using U = std::invoke_result_t<F, T&&>;
    if (hasValue()) {
      try {
        return Try<U>(std::invoke(std::forward<F>(f), std::move(value())));
      } catch (...) {
        return Try<U>(std::current_exception());
      }
    }
    if (hasException()) {
      return Try<U>(std::move(*this).exception());
    }
    return Try<U>();
  }

  // thenTry: Apply function returning Try, flatten result
  // F: T -> Try<U>
  // Returns: Try<U>
  template <typename F>
  auto thenTry(F&& f) & -> std::invoke_result_t<F, T&> {
    using ResultTry = std::invoke_result_t<F, T&>;
    if (hasValue()) {
      try {
        return std::invoke(std::forward<F>(f), value());
      } catch (...) {
        return ResultTry(std::current_exception());
      }
    }
    if (hasException()) {
      return ResultTry(exception());
    }
    return ResultTry();
  }

  template <typename F>
  auto thenTry(F&& f) && -> std::invoke_result_t<F, T&&> {
    using ResultTry = std::invoke_result_t<F, T&&>;
    if (hasValue()) {
      try {
        return std::invoke(std::forward<F>(f), std::move(value()));
      } catch (...) {
        return ResultTry(std::current_exception());
      }
    }
    if (hasException()) {
      return ResultTry(std::move(*this).exception());
    }
    return ResultTry();
  }

  // onError: Call function if exception, return *this for chaining
  template <typename F>
  Try& onError(F&& f) & {
    if (hasException()) {
      std::invoke(std::forward<F>(f), exception());
    }
    return *this;
  }

  template <typename F>
  Try&& onError(F&& f) && {
    if (hasException()) {
      std::invoke(std::forward<F>(f), exception());
    }
    return std::move(*this);
  }

  // recover: Transform exception into value
  // F: std::exception_ptr -> T
  // Returns: Try<T>
  template <typename F>
  Try recover(F&& f) && {
    if (hasValue()) {
      return std::move(*this);
    }
    if (hasException()) {
      try {
        return Try(std::invoke(std::forward<F>(f), exception()));
      } catch (...) {
        return Try(std::current_exception());
      }
    }
    return Try();  // Empty stays empty
  }

  // ==========================================================================
  // Value-or operations
  // ==========================================================================

  template <typename U>
  T value_or(U&& defaultValue) const& {
    return hasValue() ? value() : static_cast<T>(std::forward<U>(defaultValue));
  }

  template <typename U>
  T value_or(U&& defaultValue) && {
    return hasValue() ? std::move(value()) : static_cast<T>(std::forward<U>(defaultValue));
  }

 private:
  void throwIfFailed() const {
    if (hasException()) {
      std::rethrow_exception(std::get<std::exception_ptr>(storage_));
    }
    if (isEmpty()) {
      throw std::runtime_error("Try is empty");
    }
  }

  std::variant<std::monostate, T, std::exception_ptr> storage_;
};

// =============================================================================
// Try<void> specialization
// =============================================================================

template <>
class Try<void> {
 public:
  using value_type = void;

  Try() : hasValue_(true) {}
  explicit Try(std::exception_ptr ex) : exception_(std::move(ex)) {}

  Try(const Try&) = default;
  Try(Try&&) = default;
  Try& operator=(const Try&) = default;
  Try& operator=(Try&&) = default;

  bool hasValue() const noexcept { return hasValue_ && !exception_; }
  bool hasException() const noexcept { return exception_ != nullptr; }
  bool isEmpty() const noexcept { return !hasValue_ && !exception_; }
  explicit operator bool() const noexcept { return hasValue(); }

  void value() const {
    if (hasException()) {
      std::rethrow_exception(exception_);
    }
    if (!hasValue_) {
      throw std::runtime_error("Try is empty");
    }
  }

  std::exception_ptr exception() const noexcept { return exception_; }

  template <typename E>
  bool hasExceptionOf() const noexcept {
    return tryGetExceptionObject<E>() != nullptr;
  }

  template <typename E>
  const E* tryGetExceptionObject() const noexcept {
    if (!hasException()) {
      return nullptr;
    }
    try {
      std::rethrow_exception(exception_);
    } catch (const E& e) {
      return &e;
    } catch (...) {
      return nullptr;
    }
  }

  std::string exceptionMessage() const {
    if (auto* e = tryGetExceptionObject<std::exception>()) {
      return e->what();
    }
    return hasException() ? "unknown exception" : "";
  }

  // Monadic operations for void
  template <typename F>
  auto thenValue(F&& f) -> Try<std::invoke_result_t<F>> {
    using U = std::invoke_result_t<F>;
    if (hasValue()) {
      try {
        if constexpr (std::is_void_v<U>) {
          std::invoke(std::forward<F>(f));
          return Try<void>();
        } else {
          return Try<U>(std::invoke(std::forward<F>(f)));
        }
      } catch (...) {
        return Try<U>(std::current_exception());
      }
    }
    if (hasException()) {
      return Try<U>(exception());
    }
    return Try<U>();
  }

  template <typename F>
  Try& onError(F&& f) & {
    if (hasException()) {
      std::invoke(std::forward<F>(f), exception_);
    }
    return *this;
  }

 private:
  bool hasValue_{false};
  std::exception_ptr exception_;
};

// =============================================================================
// Factory functions
// =============================================================================

// Create Try from value
template <typename T>
Try<std::decay_t<T>> makeTryWith(T&& value) {
  return Try<std::decay_t<T>>(std::forward<T>(value));
}

// Create Try<void> success
inline Try<void> makeTryWith() {
  return Try<void>();
}

// Execute function and capture result/exception in Try
template <typename F>
auto makeTryWithNoThrow(F&& f) -> Try<std::invoke_result_t<F>> {
  using T = std::invoke_result_t<F>;
  try {
    if constexpr (std::is_void_v<T>) {
      std::invoke(std::forward<F>(f));
      return Try<void>();
    } else {
      return Try<T>(std::invoke(std::forward<F>(f)));
    }
  } catch (...) {
    return Try<T>(std::current_exception());
  }
}

} // namespace folly

#endif // !MOXYGEN_USE_FOLLY

// Bring into compat namespace
namespace moxygen::compat {
template <typename T>
using Try = folly::Try<T>;

using folly::makeTryWith;
using folly::makeTryWithNoThrow;
} // namespace moxygen::compat
