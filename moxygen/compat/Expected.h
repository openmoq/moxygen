/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <moxygen/compat/Config.h>

#if MOXYGEN_USE_FOLLY
#include <folly/Expected.h>
#else
#include <functional>
#include <type_traits>
#include <utility>
#include <variant>
#endif

namespace moxygen::compat {

#if MOXYGEN_USE_FOLLY

template <typename T, typename E>
using Expected = folly::Expected<T, E>;

template <typename E>
using Unexpected = folly::Unexpected<E>;

template <typename E>
auto makeUnexpected(E&& e) {
  return folly::makeUnexpected(std::forward<E>(e));
}

#else

// Forward declaration
template <typename T, typename E>
class Expected;

template <typename E>
class Unexpected {
 public:
  explicit Unexpected(E error) : error_(std::move(error)) {}

  const E& value() const& { return error_; }
  E& value() & { return error_; }
  E&& value() && { return std::move(error_); }
  const E&& value() const&& { return std::move(error_); }

 private:
  E error_;
};

template <typename E>
Unexpected<std::decay_t<E>> makeUnexpected(E&& e) {
  return Unexpected<std::decay_t<E>>(std::forward<E>(e));
}

// =============================================================================
// Expected<T, E> - Improved error handling with monadic operations
// =============================================================================
// API compatible with both folly::Expected and C++23 std::expected.
// Requires: C++17 or later (uses std::variant, std::invoke)
//
// Monadic operations:
// - then(F)      : Transform value with F, propagate error (folly style)
// - transform(F) : Same as then() (C++23 style)
// - and_then(F)  : Chain Expected-returning operations (C++23 style)
// - or_else(F)   : Handle error, potentially recover (C++23 style)
// - value_or(U)  : Get value or default (C++23 style)
// - valueOr(U)   : Same as value_or() (folly style)
// - onError(F)   : Call F on error, return *this for chaining
// =============================================================================

template <typename T, typename E>
class Expected {
 public:
  using value_type = T;
  using error_type = E;
  using unexpected_type = Unexpected<E>;

  // Default constructor - initializes with default T value
  Expected() : storage_(T{}) {}

  Expected(T value) : storage_(std::move(value)) {}
  Expected(Unexpected<E> error) : storage_(std::move(error)) {}

  // Converting constructor for types implicitly convertible to T
  template <
      typename U,
      typename = std::enable_if_t<
          std::is_convertible_v<U, T> && !std::is_same_v<std::decay_t<U>, T> &&
          !std::is_same_v<std::decay_t<U>, Expected>>>
  Expected(U&& value) : storage_(T(std::forward<U>(value))) {}

  // Copy/move constructors and assignment (defaulted)
  Expected(const Expected&) = default;
  Expected(Expected&&) = default;
  Expected& operator=(const Expected&) = default;
  Expected& operator=(Expected&&) = default;

  // ==========================================================================
  // Observers
  // ==========================================================================

  bool hasValue() const noexcept { return std::holds_alternative<T>(storage_); }
  bool hasError() const noexcept { return !hasValue(); }
  explicit operator bool() const noexcept { return hasValue(); }

  // C++23 compatibility aliases
  bool has_value() const noexcept { return hasValue(); }

  // ==========================================================================
  // Value access
  // ==========================================================================

  T& value() & {
    if (hasError()) {
      throw std::logic_error("Expected::value() called on error");
    }
    return std::get<T>(storage_);
  }
  const T& value() const& {
    if (hasError()) {
      throw std::logic_error("Expected::value() called on error");
    }
    return std::get<T>(storage_);
  }
  T&& value() && {
    if (hasError()) {
      throw std::logic_error("Expected::value() called on error");
    }
    return std::get<T>(std::move(storage_));
  }
  const T&& value() const&& {
    if (hasError()) {
      throw std::logic_error("Expected::value() called on error");
    }
    return std::get<T>(std::move(storage_));
  }

  // ==========================================================================
  // Error access
  // ==========================================================================

  E& error() & {
    return std::get<Unexpected<E>>(storage_).value();
  }
  const E& error() const& {
    return std::get<Unexpected<E>>(storage_).value();
  }
  E&& error() && {
    return std::get<Unexpected<E>>(std::move(storage_)).value();
  }
  const E&& error() const&& {
    return std::get<Unexpected<E>>(std::move(storage_)).value();
  }

  // ==========================================================================
  // Dereference operators (unchecked access)
  // ==========================================================================

  T* operator->() { return &std::get<T>(storage_); }
  const T* operator->() const { return &std::get<T>(storage_); }
  T& operator*() & { return std::get<T>(storage_); }
  const T& operator*() const& { return std::get<T>(storage_); }
  T&& operator*() && { return std::get<T>(std::move(storage_)); }

  // ==========================================================================
  // value_or / valueOr - Get value or default
  // ==========================================================================

  template <typename U>
  T value_or(U&& defaultValue) const& {
    return hasValue() ? value() : static_cast<T>(std::forward<U>(defaultValue));
  }

  template <typename U>
  T value_or(U&& defaultValue) && {
    return hasValue() ? std::move(value()) : static_cast<T>(std::forward<U>(defaultValue));
  }

  // Folly-style alias
  template <typename U>
  T valueOr(U&& defaultValue) const& {
    return value_or(std::forward<U>(defaultValue));
  }

  template <typename U>
  T valueOr(U&& defaultValue) && {
    return std::move(*this).value_or(std::forward<U>(defaultValue));
  }

  // ==========================================================================
  // error_or - Get error or default
  // ==========================================================================

  template <typename U>
  E error_or(U&& defaultError) const& {
    return hasError() ? error() : static_cast<E>(std::forward<U>(defaultError));
  }

  template <typename U>
  E error_or(U&& defaultError) && {
    return hasError() ? std::move(error()) : static_cast<E>(std::forward<U>(defaultError));
  }

  // ==========================================================================
  // then / transform - Map value, propagate error
  // ==========================================================================
  // F: T -> U
  // Returns: Expected<U, E>

  template <typename F>
  auto then(F&& f) & -> Expected<std::invoke_result_t<F, T&>, E> {
    using U = std::invoke_result_t<F, T&>;
    if (hasValue()) {
      return Expected<U, E>(std::invoke(std::forward<F>(f), value()));
    }
    return Expected<U, E>(makeUnexpected(error()));
  }

  template <typename F>
  auto then(F&& f) const& -> Expected<std::invoke_result_t<F, const T&>, E> {
    using U = std::invoke_result_t<F, const T&>;
    if (hasValue()) {
      return Expected<U, E>(std::invoke(std::forward<F>(f), value()));
    }
    return Expected<U, E>(makeUnexpected(error()));
  }

  template <typename F>
  auto then(F&& f) && -> Expected<std::invoke_result_t<F, T&&>, E> {
    using U = std::invoke_result_t<F, T&&>;
    if (hasValue()) {
      return Expected<U, E>(std::invoke(std::forward<F>(f), std::move(value())));
    }
    return Expected<U, E>(makeUnexpected(std::move(error())));
  }

  // C++23 style alias
  template <typename F>
  auto transform(F&& f) & { return then(std::forward<F>(f)); }

  template <typename F>
  auto transform(F&& f) const& { return then(std::forward<F>(f)); }

  template <typename F>
  auto transform(F&& f) && { return std::move(*this).then(std::forward<F>(f)); }

  // ==========================================================================
  // and_then - Chain Expected-returning operations
  // ==========================================================================
  // F: T -> Expected<U, E>
  // Returns: Expected<U, E>

  template <typename F>
  auto and_then(F&& f) & -> std::invoke_result_t<F, T&> {
    if (hasValue()) {
      return std::invoke(std::forward<F>(f), value());
    }
    using ReturnType = std::invoke_result_t<F, T&>;
    return ReturnType(makeUnexpected(error()));
  }

  template <typename F>
  auto and_then(F&& f) const& -> std::invoke_result_t<F, const T&> {
    if (hasValue()) {
      return std::invoke(std::forward<F>(f), value());
    }
    using ReturnType = std::invoke_result_t<F, const T&>;
    return ReturnType(makeUnexpected(error()));
  }

  template <typename F>
  auto and_then(F&& f) && -> std::invoke_result_t<F, T&&> {
    if (hasValue()) {
      return std::invoke(std::forward<F>(f), std::move(value()));
    }
    using ReturnType = std::invoke_result_t<F, T&&>;
    return ReturnType(makeUnexpected(std::move(error())));
  }

  // ==========================================================================
  // or_else - Handle error, potentially recover
  // ==========================================================================
  // F: E -> Expected<T, E2>  (can change error type or recover to value)
  // Returns: Expected<T, E2>

  template <typename F>
  auto or_else(F&& f) & -> std::invoke_result_t<F, E&> {
    if (hasValue()) {
      using ReturnType = std::invoke_result_t<F, E&>;
      return ReturnType(value());
    }
    return std::invoke(std::forward<F>(f), error());
  }

  template <typename F>
  auto or_else(F&& f) const& -> std::invoke_result_t<F, const E&> {
    if (hasValue()) {
      using ReturnType = std::invoke_result_t<F, const E&>;
      return ReturnType(value());
    }
    return std::invoke(std::forward<F>(f), error());
  }

  template <typename F>
  auto or_else(F&& f) && -> std::invoke_result_t<F, E&&> {
    if (hasValue()) {
      using ReturnType = std::invoke_result_t<F, E&&>;
      return ReturnType(std::move(value()));
    }
    return std::invoke(std::forward<F>(f), std::move(error()));
  }

  // ==========================================================================
  // transform_error - Map error, propagate value
  // ==========================================================================
  // F: E -> E2
  // Returns: Expected<T, E2>

  template <typename F>
  auto transform_error(F&& f) & -> Expected<T, std::invoke_result_t<F, E&>> {
    using E2 = std::invoke_result_t<F, E&>;
    if (hasValue()) {
      return Expected<T, E2>(value());
    }
    return Expected<T, E2>(makeUnexpected(std::invoke(std::forward<F>(f), error())));
  }

  template <typename F>
  auto transform_error(F&& f) const& -> Expected<T, std::invoke_result_t<F, const E&>> {
    using E2 = std::invoke_result_t<F, const E&>;
    if (hasValue()) {
      return Expected<T, E2>(value());
    }
    return Expected<T, E2>(makeUnexpected(std::invoke(std::forward<F>(f), error())));
  }

  template <typename F>
  auto transform_error(F&& f) && -> Expected<T, std::invoke_result_t<F, E&&>> {
    using E2 = std::invoke_result_t<F, E&&>;
    if (hasValue()) {
      return Expected<T, E2>(std::move(value()));
    }
    return Expected<T, E2>(makeUnexpected(std::invoke(std::forward<F>(f), std::move(error()))));
  }

  // ==========================================================================
  // onError - Call function on error, return *this for chaining (folly style)
  // ==========================================================================

  template <typename F>
  Expected& onError(F&& f) & {
    if (hasError()) {
      std::invoke(std::forward<F>(f), error());
    }
    return *this;
  }

  template <typename F>
  const Expected& onError(F&& f) const& {
    if (hasError()) {
      std::invoke(std::forward<F>(f), error());
    }
    return *this;
  }

  template <typename F>
  Expected&& onError(F&& f) && {
    if (hasError()) {
      std::invoke(std::forward<F>(f), error());
    }
    return std::move(*this);
  }

  // ==========================================================================
  // Comparison operators
  // ==========================================================================

  template <typename T2, typename E2>
  bool operator==(const Expected<T2, E2>& other) const {
    if (hasValue() != other.hasValue()) {
      return false;
    }
    if (hasValue()) {
      return value() == other.value();
    }
    return error() == other.error();
  }

  template <typename T2, typename E2>
  bool operator!=(const Expected<T2, E2>& other) const {
    return !(*this == other);
  }

 private:
  std::variant<T, Unexpected<E>> storage_;
};

#endif

} // namespace moxygen::compat
