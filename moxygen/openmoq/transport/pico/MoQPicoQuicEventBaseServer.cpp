/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "moxygen/openmoq/transport/pico/MoQPicoQuicEventBaseServer.h"
#include <folly/logging/xlog.h>
#include <moxygen/events/MoQFollyExecutorImpl.h>
#include <moxygen/openmoq/transport/pico/PicoQuicSocketHandler.h>
#include <picoquic.h>

namespace moxygen {

struct MoQPicoQuicEventBaseServer::Impl {
  std::unique_ptr<PicoQuicSocketHandler> handler;
  std::atomic<bool> running_{false};
};

MoQPicoQuicEventBaseServer::MoQPicoQuicEventBaseServer(
    std::string cert,
    std::string key,
    std::string endpoint,
    folly::EventBase* evb,
    MoQFollyExecutorImpl* executor,
    std::string versions)
    : MoQPicoServerBase(
          std::move(cert),
          std::move(key),
          std::move(endpoint),
          std::move(versions)),
      impl_(std::make_unique<Impl>()),
      evb_(evb),
      extExecutor_(executor) {}

MoQPicoQuicEventBaseServer::~MoQPicoQuicEventBaseServer() {
  stop();
}

void MoQPicoQuicEventBaseServer::start(const folly::SocketAddress& addr) {
  if (impl_->running_.exchange(true)) {
    XLOG(WARN) << "Server already running";
    return;
  }

  // Wrap the caller-supplied executor in a shared_ptr with a no-op deleter
  // so it can be passed to createSession() which takes shared_ptr<MoQExecutor>.
  // The caller retains ownership; this server must not outlive it.
  executor_ = std::shared_ptr<MoQExecutor>(
      static_cast<MoQExecutor*>(extExecutor_), [](MoQExecutor*) {});

  if (!createQuicContext()) {
    executor_.reset();
    impl_->running_ = false;
    return;
  }

  XLOG(INFO) << "Starting MoQPicoQuicEventBaseServer on " << addr.describe();

  impl_->handler = std::make_unique<PicoQuicSocketHandler>(evb_, quic_);
  impl_->handler->start(addr);
}

void MoQPicoQuicEventBaseServer::stop() {
  if (!impl_->running_.exchange(false)) {
    return;
  }

  XLOG(INFO) << "Stopping MoQPicoQuicEventBaseServer";

  if (impl_->handler) {
    impl_->handler->stop();
    impl_->handler.reset();
  }

  destroyQuicContext();
  executor_.reset();

  XLOG(INFO) << "MoQPicoQuicEventBaseServer stopped";
}

} // namespace moxygen
