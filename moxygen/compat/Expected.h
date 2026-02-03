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

template <typename T, typename E>
class Expected {
 public:
  Expected(T value) : storage_(std::move(value)) {}
  Expected(Unexpected<E> error) : storage_(std::move(error)) {}

  bool hasValue() const { return std::holds_alternative<T>(storage_); }
  bool hasError() const { return !hasValue(); }
  explicit operator bool() const { return hasValue(); }

  T& value() & {
    return std::get<T>(storage_);
  }
  const T& value() const& {
    return std::get<T>(storage_);
  }
  T&& value() && {
    return std::get<T>(std::move(storage_));
  }
  const T&& value() const&& {
    return std::get<T>(std::move(storage_));
  }

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

  T* operator->() { return &value(); }
  const T* operator->() const { return &value(); }
  T& operator*() & { return value(); }
  const T& operator*() const& { return value(); }
  T&& operator*() && { return std::move(value()); }

 private:
  std::variant<T, Unexpected<E>> storage_;
};

#endif

} // namespace moxygen::compat
