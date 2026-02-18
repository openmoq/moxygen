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
#include <folly/DefaultKeepAliveExecutor.h>
#if MOXYGEN_QUIC_MVFST
#include <quic/common/events/QuicEventBase.h>
#endif
#endif

namespace moxygen {

#if MOXYGEN_USE_FOLLY && MOXYGEN_QUIC_MVFST

// Folly + mvfst mode: inherit from DefaultKeepAliveExecutor, use QuicTimerCallback
class MoQExecutor : public folly::DefaultKeepAliveExecutor {
 public:
  using KeepAlive = folly::Executor::KeepAlive<MoQExecutor>;

  virtual ~MoQExecutor() override = default;

  // Returns a KeepAlive token for this executor
  KeepAlive keepAlive() {
    return getKeepAliveToken(this);
  }

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
  // KeepAlive type for Folly+picoquic - use shared_ptr for ownership
  using KeepAlive = std::shared_ptr<MoQExecutor>;

  virtual ~MoQExecutor() = default;

  // Execute a function - use std::function for compatibility with std-mode
  // implementations. This hides folly::Executor::add(Func) which uses
  // folly::Function, but allows consistent interface across modes.
  virtual void add(std::function<void()> func) = 0;

  // Implement folly::Executor::add by forwarding to our add
  // Use shared_ptr wrapper since std::function requires copyable callables
  // but folly::Function is move-only
  void add(folly::Function<void()> func) override {
    auto wrapper = std::make_shared<folly::Function<void()>>(std::move(func));
    add(std::function<void()>([wrapper]() { (*wrapper)(); }));
  }

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
  // KeepAlive type for std-mode - just a shared_ptr
  using KeepAlive = std::shared_ptr<MoQExecutor>;

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
