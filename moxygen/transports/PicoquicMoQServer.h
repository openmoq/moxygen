/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <moxygen/compat/Config.h>

#if MOXYGEN_QUIC_PICOQUIC

#include <moxygen/compat/MoQServerInterface.h>
#include <moxygen/transports/PicoquicRawTransport.h>
#include <moxygen/transports/TransportMode.h>

// Forward declaration
namespace moxygen {
class MoQSession;
}

#include <picoquic.h>

// h3zero headers for WebTransport support
extern "C" {
#include <h3zero.h>
#include <h3zero_common.h>
}

#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>

namespace moxygen::transports {

// Forward declaration
class PicoquicH3Transport;

/**
 * PicoquicMoQServer implements MoQServerInterface using picoquic.
 * This provides a pure C++ implementation without Folly/Proxygen dependencies.
 *
 * Supports two transport modes:
 * - QUIC: Direct MoQ over QUIC (raw QUIC with "moq-00" ALPN)
 * - WEBTRANSPORT: MoQ over WebTransport over HTTP/3 ("h3" ALPN)
 *
 * WebTransport mode enables interoperability with mvfst/proxygen clients.
 */
class PicoquicMoQServer : public compat::MoQServerInterface {
 public:
  struct Config {
    std::string certFile;  // Server certificate (required)
    std::string keyFile;   // Server private key (required)
    std::string alpn;      // If empty, auto-selected based on transport mode

    // Transport mode selection
    TransportMode transportMode{TransportMode::QUIC};

    // Connection settings
    std::chrono::milliseconds idleTimeout{30000};
    size_t maxStreamData{1000000};
    size_t maxData{10000000};
    size_t maxConnections{100};
  };

  PicoquicMoQServer(
      std::shared_ptr<MoQExecutor> executor,
      Config config,
      std::string endpoint = "/moq");

  ~PicoquicMoQServer() override;

  // MoQServerInterface implementation
  void start(
      const compat::SocketAddress& addr,
      std::shared_ptr<SessionHandler> handler) override;

  void stop() override;

  [[nodiscard]] bool isRunning() const override {
    return running_;
  }

  std::shared_ptr<MoQExecutor> getExecutor() const override {
    return executor_;
  }

  // Access server configuration
  const Config& getConfig() const {
    return config_;
  }

 private:
  // Forward declaration for setup callback impl
  class ServerSetupCallbackImpl;

  // Per-connection state
  struct ConnectionState {
    std::shared_ptr<PicoquicRawTransport> transport;  // Raw QUIC mode
    std::shared_ptr<PicoquicH3Transport> h3Transport; // WebTransport mode
    std::shared_ptr<MoQSession> session;
    std::shared_ptr<ServerSetupCallbackImpl> setupCallback;
  };

  // picoquic callback for all connections (raw QUIC mode)
  static int picoquicCallback(
      picoquic_cnx_t* cnx,
      uint64_t stream_id,
      uint8_t* bytes,
      size_t length,
      picoquic_call_back_event_t event,
      void* callback_ctx,
      void* stream_ctx);

  // WebTransport path callback (WebTransport mode)
  static int wtPathCallback(
      picoquic_cnx_t* cnx,
      uint8_t* bytes,
      size_t length,
      picohttp_call_back_event_t event,
      struct st_h3zero_stream_ctx_t* stream_ctx,
      void* callback_ctx);

  // Loop callback for network thread events
  static int picoLoopCallback(
      picoquic_quic_t* quic,
      picoquic_packet_loop_cb_enum cb_mode,
      void* callback_ctx,
      void* callback_argv);

  // Initialize picoquic context
  bool initQuicContext();

  // Handle new connection
  void onNewConnection(picoquic_cnx_t* cnx);

  // Handle connection close
  void onConnectionClose(picoquic_cnx_t* cnx);

  std::shared_ptr<MoQExecutor> executor_;
  Config config_;
  std::string endpoint_;

  // picoquic state
  picoquic_quic_t* quic_{nullptr};

  // h3zero state for WebTransport mode
  h3zero_callback_ctx_t* h3Ctx_{nullptr};
  picohttp_server_path_item_t pathTable_[1];
  picohttp_server_parameters_t serverParams_{};

  // Session handler
  std::shared_ptr<SessionHandler> sessionHandler_;

  // Active connections (accessed only from pico thread after start)
  std::unordered_map<picoquic_cnx_t*, ConnectionState> connections_;

  // Server state
  std::atomic<bool> running_{false};

  // Network thread
  PicoquicThreadDispatcher dispatcher_;
  picoquic_network_thread_ctx_t* threadCtx_{nullptr};
  picoquic_packet_loop_param_t loopParam_{};
};

/**
 * PicoquicMoQServerFactory creates PicoquicMoQServer instances
 */
class PicoquicMoQServerFactory : public compat::MoQServerFactory {
 public:
  explicit PicoquicMoQServerFactory(PicoquicMoQServer::Config config)
      : config_(std::move(config)) {}

  std::unique_ptr<compat::MoQServerInterface> createServer(
      std::shared_ptr<MoQExecutor> executor,
      const std::string& endpoint) override;

 private:
  PicoquicMoQServer::Config config_;
};

} // namespace moxygen::transports

#endif // MOXYGEN_QUIC_PICOQUIC
