/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <moxygen/compat/Config.h>

#if MOXYGEN_QUIC_PICOQUIC

#include <moxygen/compat/MoQClientInterface.h>
#include <moxygen/transports/openmoq/picoquic/PicoquicRawTransport.h>
#include <moxygen/transports/openmoq/picoquic/TransportMode.h>

// Forward declaration
namespace moxygen {
class MoQSession;
}

#include <picoquic.h>

#include <atomic>
#include <memory>
#include <string>

namespace moxygen::transports {

// Forward declaration
class PicoquicH3Transport;

/**
 * PicoquicMoQClient implements MoQClientInterface using picoquic.
 * This provides a pure C++ implementation without Folly/Proxygen dependencies.
 *
 * Supports two transport modes:
 * - QUIC: Direct MoQ over QUIC (raw QUIC with "moq-00" ALPN)
 * - WEBTRANSPORT: MoQ over WebTransport over HTTP/3 ("h3" ALPN)
 *
 * WebTransport mode enables interoperability with mvfst/proxygen servers.
 */
class PicoquicMoQClient : public compat::MoQClientInterface {
 public:
  // Session factory function type
  using SessionFactory = std::function<std::shared_ptr<MoQSession>(
      std::shared_ptr<compat::WebTransportInterface>,
      std::shared_ptr<MoQExecutor>)>;

  struct Config {
    std::string serverHost;
    uint16_t serverPort{443};
    std::string alpn;  // If empty, auto-selected based on transport mode
    std::string sni;   // If empty, uses serverHost

    // Transport mode selection
    TransportMode transportMode{TransportMode::QUIC};
    std::string wtPath{"/moq"};  // WebTransport endpoint path

    // TLS settings
    std::string certFile;    // Client certificate (optional)
    std::string keyFile;     // Client key (optional)
    std::string caFile;      // CA for server validation (optional)
    bool insecure{false};    // Skip server certificate validation

    // Connection settings
    std::chrono::milliseconds idleTimeout{30000};
    size_t maxStreamData{1000000};
    size_t maxData{10000000};

    // Session factory (optional) - if not set, creates regular MoQSession
    // Use MoQRelaySession::createRelaySessionFactory() for relay clients
    SessionFactory sessionFactory;
  };

  PicoquicMoQClient(
      std::shared_ptr<MoQExecutor> executor,
      Config config);

  ~PicoquicMoQClient() override;

  // Picoquic transport uses callback-based API even in Folly mode
  // (coroutine API is only available with mvfst/proxygen)
  void connectWithCallback(
      std::chrono::milliseconds timeout,
      std::shared_ptr<Publisher> publishHandler,
      std::shared_ptr<Subscriber> subscribeHandler,
      std::shared_ptr<compat::ResultCallback<ConnectResult, ConnectError>>
          callback) override;

  std::shared_ptr<MoQSession> getSession() const override;

  void close(SessionCloseErrorCode errorCode) override;

  std::shared_ptr<MoQExecutor> getExecutor() const override {
    return executor_;
  }

  // Access the underlying transport
  PicoquicRawTransport* getTransport() const {
    return transport_.get();
  }

 private:
  // picoquic callback for connection events
  static int picoquicCallback(
      picoquic_cnx_t* cnx,
      uint64_t stream_id,
      uint8_t* bytes,
      size_t length,
      picoquic_call_back_event_t event,
      void* callback_ctx,
      void* stream_ctx);

  // Loop callback for network thread events
  static int picoLoopCallback(
      picoquic_quic_t* quic,
      picoquic_packet_loop_cb_enum cb_mode,
      void* callback_ctx,
      void* callback_argv);

  // Initialize picoquic context
  bool initQuicContext();

  // Handle connection established
  void onConnected();

  // Handle connection error
  void onConnectionError(uint32_t errorCode);

  // Complete session setup (called after transport is ready)
  void completeSessionSetup(
      compat::WebTransportInterface* wtInterface,
      std::shared_ptr<compat::WebTransportInterface> wtShared);

  std::shared_ptr<MoQExecutor> executor_;
  Config config_;
  std::string computedAlpn_;  // ALPN value computed in initQuicContext()

  // picoquic state
  picoquic_quic_t* quic_{nullptr};
  picoquic_cnx_t* cnx_{nullptr};

  // Transport wrapper (one of these will be used based on mode)
  std::shared_ptr<PicoquicRawTransport> transport_;      // For QUIC mode
  std::shared_ptr<PicoquicH3Transport> h3Transport_;     // For WebTransport mode

  // MoQ session
  std::shared_ptr<MoQSession> session_;

  // Connection state
  std::atomic<bool> connected_{false};
  std::atomic<bool> closed_{false};

  // Pending connect callback (for callback-based connect)
  std::shared_ptr<compat::ResultCallback<ConnectResult, ConnectError>>
      connectCallback_;
  std::shared_ptr<Publisher> pendingPublishHandler_;
  std::shared_ptr<Subscriber> pendingSubscribeHandler_;

  // Network thread
  PicoquicThreadDispatcher dispatcher_;
  picoquic_network_thread_ctx_t* threadCtx_{nullptr};
  picoquic_packet_loop_param_t loopParam_{};
};

/**
 * PicoquicMoQClientFactory creates PicoquicMoQClient instances
 */
class PicoquicMoQClientFactory : public compat::MoQClientFactory {
 public:
  explicit PicoquicMoQClientFactory(PicoquicMoQClient::Config defaultConfig = {})
      : defaultConfig_(std::move(defaultConfig)) {}

  std::unique_ptr<compat::MoQClientInterface> createClient(
      std::shared_ptr<MoQExecutor> executor,
      const std::string& url) override;

 private:
  PicoquicMoQClient::Config defaultConfig_;
};

} // namespace moxygen::transports

#endif // MOXYGEN_QUIC_PICOQUIC
