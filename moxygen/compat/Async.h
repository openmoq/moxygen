/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <moxygen/compat/Config.h>

#if MOXYGEN_USE_FOLLY
#include <folly/coro/Task.h>
#include <folly/futures/Future.h>
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
// The Task and SemiFuture types are not available.
// See Callbacks.h for callback interfaces.

#endif

} // namespace moxygen::compat
