/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/Executor.h>
#include <quic/common/events/QuicEventBase.h>
#include <chrono>

namespace moxygen {

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

  // Token that keeps the backing loop alive while held. Base returns a
  // non-owning dummy (Executor::keepAliveAcquire() defaults to false); an
  // EventBase-backed executor overrides this with a real, cross-thread token.
  virtual folly::Executor::KeepAlive<> getKeepAlive() {
    return folly::getKeepAliveToken(static_cast<folly::Executor*>(this));
  }
};

} // namespace moxygen
