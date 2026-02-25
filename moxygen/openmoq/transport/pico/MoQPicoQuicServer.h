/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/SocketAddress.h>
#include <memory>
#include <moxygen/MoQServerBase.h>
#include <moxygen/MoQSession.h>
#include <moxygen/mlog/MLogger.h>
#include <moxygen/openmoq/transport/pico/PicoQuicExecutor.h>
#include <picoquic.h>
#include <string>
#include <thread>

namespace moxygen {

/**
 * MoQPicoQuicServer - MoQ server using picoquic transport
 *
 * This server creates WebTransport sessions over raw QUIC using picoquic,
 * as opposed to MoQServer which uses HTTP/3 WebTransport over proxygen.
 */
class MoQPicoQuicServer : public MoQServerBase {
public:
  MoQPicoQuicServer(std::string cert, std::string key, std::string endpoint);

  MoQPicoQuicServer(const MoQPicoQuicServer &) = delete;
  MoQPicoQuicServer(MoQPicoQuicServer &&) = delete;
  MoQPicoQuicServer &operator=(const MoQPicoQuicServer &) = delete;
  MoQPicoQuicServer &operator=(MoQPicoQuicServer &&) = delete;
  ~MoQPicoQuicServer() override;

  /**
   * Start the server on the specified address.
   * This will create a background thread running the picoquic packet loop.
   */
  void start(const folly::SocketAddress &addr);

  /**
   * Stop the server and wait for the packet loop thread to finish.
   */
  void stop();

private:
  // Picoquic callback - static function that routes to instance methods
  static int picoCallback(picoquic_cnx_t *cnx, uint64_t stream_id,
                          uint8_t *bytes, size_t length,
                          picoquic_call_back_event_t fin_or_event,
                          void *callback_ctx, void *v_stream_ctx);

  // Handle new connection event
  void onNewConnection(picoquic_cnx_t *cnx);

  // Server parameters
  std::string cert_;
  std::string key_;

  // Picoquic context
  picoquic_quic_t *quic_{nullptr};

  // Network thread context and parameters
  picoquic_packet_loop_param_t loopParam_{};
  picoquic_network_thread_ctx_t *networkThreadCtx_{nullptr};

  // Server state
  folly::SocketAddress serverAddr_;
  std::atomic<bool> running_{false};

  // Executor for all sessions
  std::shared_ptr<PicoQuicExecutor> executor_;
};

} // namespace moxygen
