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
#include <chrono>
#include <future>
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

// In std mode, async operations use callbacks instead of coroutines/futures.
// See Callbacks.h for callback interfaces.

// Simple SemiFuture stub for std-mode (wraps std::future)
template <typename T>
class SemiFuture {
 public:
  SemiFuture() = default;
  explicit SemiFuture(T value) : value_(std::move(value)), ready_(true) {}
  explicit SemiFuture(std::future<T> future) : future_(std::move(future)) {}

  bool isReady() const {
    return ready_ ||
        (future_.valid() &&
         future_.wait_for(std::chrono::seconds(0)) == std::future_status::ready);
  }

  T value() {
    if (ready_) {
      return std::move(value_);
    }
    return future_.get();
  }

 private:
  T value_;
  bool ready_{false};
  mutable std::future<T> future_;
};

template <typename T>
SemiFuture<T> makeSemiFuture(T&& val) {
  return SemiFuture<T>(std::forward<T>(val));
}

inline SemiFuture<Unit> makeSemiFuture() {
  return SemiFuture<Unit>(unit);
}

#endif

} // namespace moxygen::compat
