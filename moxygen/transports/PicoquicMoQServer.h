/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <moxygen/compat/Config.h>

#if MOXYGEN_QUIC_PICOQUIC

#include <moxygen/compat/MoQServerInterface.h>
#include <moxygen/transports/PicoquicWebTransport.h>

// Forward declaration
namespace moxygen {
class MoQSession;
}

#include <picoquic.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace moxygen::transports {

/**
 * PicoquicMoQServer implements MoQServerInterface using picoquic.
 * This provides a pure C++ implementation without Folly/Proxygen dependencies.
 */
class PicoquicMoQServer : public compat::MoQServerInterface {
 public:
  struct Config {
    std::string certFile;  // Server certificate (required)
    std::string keyFile;   // Server private key (required)
    std::string alpn{"moq-00"};

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
    std::shared_ptr<PicoquicWebTransport> transport;
    std::shared_ptr<MoQSession> session;
    std::shared_ptr<ServerSetupCallbackImpl> setupCallback;
  };

  // picoquic callback for all connections
  static int picoquicCallback(
      picoquic_cnx_t* cnx,
      uint64_t stream_id,
      uint8_t* bytes,
      size_t length,
      picoquic_call_back_event_t event,
      void* callback_ctx,
      void* stream_ctx);

  // Initialize picoquic context
  bool initQuicContext();

  // Run the server event loop (blocking)
  void runServer(const compat::SocketAddress& addr);

  // Handle new connection
  void onNewConnection(picoquic_cnx_t* cnx);

  // Handle connection close
  void onConnectionClose(picoquic_cnx_t* cnx);

  std::shared_ptr<MoQExecutor> executor_;
  Config config_;
  std::string endpoint_;

  // picoquic state
  picoquic_quic_t* quic_{nullptr};

  // Session handler
  std::shared_ptr<SessionHandler> sessionHandler_;

  // Active connections
  std::mutex connectionsMutex_;
  std::unordered_map<picoquic_cnx_t*, ConnectionState> connections_;

  // Server state
  std::atomic<bool> running_{false};
  std::thread serverThread_;
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
