/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <moxygen/compat/Config.h>
#include <chrono>
#include <functional>

#if MOXYGEN_USE_FOLLY
#include <folly/Executor.h>
#if MOXYGEN_QUIC_MVFST
#include <quic/common/events/QuicEventBase.h>
#endif
#endif

namespace moxygen {

#if MOXYGEN_USE_FOLLY && MOXYGEN_QUIC_MVFST

// Folly + mvfst mode: inherit from folly::Executor, use QuicTimerCallback
class MoQExecutor : public folly::Executor {
 public:
  virtual ~MoQExecutor() = default;

  template <
      typename T,
      typename = std::enable_if_t<std::is_base_of_v<MoQExecutor, T>>>
  T* getTypedExecutor() {
    auto exec = dynamic_cast<T*>(this);
    if (exec) {
      return exec;
    } else {
      return nullptr;
    }
  }

  // Timeout scheduling methods
  virtual void scheduleTimeout(
      quic::QuicTimerCallback* callback,
      std::chrono::milliseconds timeout) = 0;
};

#elif MOXYGEN_USE_FOLLY // Folly + picoquic

// Folly + picoquic mode: inherit from folly::Executor, use std::function
class MoQExecutor : public folly::Executor {
 public:
  virtual ~MoQExecutor() = default;

  template <
      typename T,
      typename = std::enable_if_t<std::is_base_of_v<MoQExecutor, T>>>
  T* getTypedExecutor() {
    auto exec = dynamic_cast<T*>(this);
    if (exec) {
      return exec;
    } else {
      return nullptr;
    }
  }

  // Timeout scheduling methods (function-based for picoquic)
  virtual void scheduleTimeout(
      std::function<void()> callback,
      std::chrono::milliseconds timeout) = 0;
};

#else // !MOXYGEN_USE_FOLLY

// Std-mode executor interface
class MoQExecutor {
 public:
  virtual ~MoQExecutor() = default;

  // Execute a function
  virtual void add(std::function<void()> func) = 0;

  template <
      typename T,
      typename = std::enable_if_t<std::is_base_of_v<MoQExecutor, T>>>
  T* getTypedExecutor() {
    auto exec = dynamic_cast<T*>(this);
    if (exec) {
      return exec;
    } else {
      return nullptr;
    }
  }

  // Timeout scheduling methods
  virtual void scheduleTimeout(
      std::function<void()> callback,
      std::chrono::milliseconds timeout) = 0;
};

#endif // MOXYGEN_USE_FOLLY

} // namespace moxygen
