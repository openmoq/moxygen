/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <moxygen/compat/Config.h>

#if MOXYGEN_QUIC_PICOQUIC

#include <moxygen/compat/MoQClientInterface.h>
#include <moxygen/transports/PicoquicWebTransport.h>

// Forward declaration
namespace moxygen {
class MoQSession;
}

#include <picoquic.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace moxygen::transports {

/**
 * PicoquicMoQClient implements MoQClientInterface using picoquic.
 * This provides a pure C++ implementation without Folly/Proxygen dependencies.
 */
class PicoquicMoQClient : public compat::MoQClientInterface {
 public:
  struct Config {
    std::string serverHost;
    uint16_t serverPort{443};
    std::string alpn{"moq-00"};
    std::string sni;  // If empty, uses serverHost

    // TLS settings
    std::string certFile;    // Client certificate (optional)
    std::string keyFile;     // Client key (optional)
    std::string caFile;      // CA for server validation (optional)
    bool insecure{false};    // Skip server certificate validation

    // Connection settings
    std::chrono::milliseconds idleTimeout{30000};
    size_t maxStreamData{1000000};
    size_t maxData{10000000};
  };

  PicoquicMoQClient(
      std::shared_ptr<MoQExecutor> executor,
      Config config);

  ~PicoquicMoQClient() override;

#if MOXYGEN_USE_FOLLY
  compat::Task<compat::Expected<ConnectResult, ConnectError>> connect(
      std::chrono::milliseconds timeout,
      std::shared_ptr<Publisher> publishHandler,
      std::shared_ptr<Subscriber> subscribeHandler) override;
#else
  void connectWithCallback(
      std::chrono::milliseconds timeout,
      std::shared_ptr<Publisher> publishHandler,
      std::shared_ptr<Subscriber> subscribeHandler,
      std::shared_ptr<compat::ResultCallback<ConnectResult, ConnectError>>
          callback) override;
#endif

  std::shared_ptr<MoQSession> getSession() const override;

  void close(SessionCloseErrorCode errorCode) override;

  std::shared_ptr<MoQExecutor> getExecutor() const override {
    return executor_;
  }

  // Access the underlying transport
  PicoquicWebTransport* getTransport() const {
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

  // Initialize picoquic context
  bool initQuicContext();

  // Run the QUIC connection (blocking)
  void runConnection();

  // Handle connection established
  void onConnected();

  // Handle connection error
  void onConnectionError(uint32_t errorCode);

  std::shared_ptr<MoQExecutor> executor_;
  Config config_;

  // picoquic state
  picoquic_quic_t* quic_{nullptr};
  picoquic_cnx_t* cnx_{nullptr};

  // Transport wrapper
  std::shared_ptr<PicoquicWebTransport> transport_;

  // MoQ session
  std::shared_ptr<MoQSession> session_;

  // Connection state
  std::atomic<bool> connected_{false};
  std::atomic<bool> closed_{false};

  // Pending connect callback (for callback-based connect)
#if !MOXYGEN_USE_FOLLY
  std::shared_ptr<compat::ResultCallback<ConnectResult, ConnectError>>
      connectCallback_;
  std::shared_ptr<Publisher> pendingPublishHandler_;
  std::shared_ptr<Subscriber> pendingSubscribeHandler_;
#endif

  // Event loop thread for picoquic
  std::thread eventLoopThread_;
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
