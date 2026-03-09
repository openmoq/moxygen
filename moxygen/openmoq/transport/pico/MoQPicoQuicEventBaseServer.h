/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/SocketAddress.h>
#include <folly/io/async/EventBase.h>
#include <memory>
#include <moxygen/openmoq/transport/pico/MoQPicoServerBase.h>
#include <string>

namespace moxygen {

class MoQFollyExecutorImpl;

/**
 * MoQPicoQuicEventBaseServer - MoQ server using picoquic on a folly::EventBase.
 *
 * Runs picoquic I/O directly on a caller-supplied EventBase rather than in a
 * dedicated background thread. This allows picoquic sessions to share an event
 * loop with H3 connections or other EventBase work.
 *
 * The caller supplies both the EventBase and a MoQFollyExecutorImpl backed by
 * that same EventBase. Both must outlive this server object.
 *
 * Usage:
 *   folly::EventBase evb;
 *   MoQFollyExecutorImpl exec(&evb);
 *   MoQPicoQuicEventBaseServer server(cert, key, endpoint, &evb, &exec);
 *   server.start(addr);
 *   evb.loop();
 */
class MoQPicoQuicEventBaseServer : public MoQPicoServerBase {
 public:
  /**
   * evb and executor must be backed by the same EventBase and must outlive
   * this server object.
   */
  MoQPicoQuicEventBaseServer(std::string cert,
                              std::string key,
                              std::string endpoint,
                              folly::EventBase* evb,
                              MoQFollyExecutorImpl* executor,
                              std::string versions = "");

  MoQPicoQuicEventBaseServer(const MoQPicoQuicEventBaseServer&) = delete;
  MoQPicoQuicEventBaseServer(MoQPicoQuicEventBaseServer&&) = delete;
  MoQPicoQuicEventBaseServer& operator=(const MoQPicoQuicEventBaseServer&) =
      delete;
  MoQPicoQuicEventBaseServer& operator=(MoQPicoQuicEventBaseServer&&) = delete;
  ~MoQPicoQuicEventBaseServer() override;

  /**
   * Bind the UDP socket and start receiving.
   * Must be called from the EventBase thread (or before evb.loop()).
   */
  void start(const folly::SocketAddress& addr) override;

  /**
   * Stop the server: cancel the wake timer, pause socket reads, free picoquic.
   * Must be called from the EventBase thread.
   */
  void stop() override;

 protected:
  void onWebTransportCreated(PicoQuicWebTransport& wt) noexcept override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;

  folly::EventBase* evb_;             // non-owning
  MoQFollyExecutorImpl* extExecutor_; // non-owning; aliased into executor_
};

} // namespace moxygen
