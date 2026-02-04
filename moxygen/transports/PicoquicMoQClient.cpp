/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "moxygen/transports/PicoquicMoQClient.h"

#if MOXYGEN_QUIC_PICOQUIC

#include <moxygen/MoQSession.h>
#include <moxygen/MoQTypes.h>
#include <moxygen/MoQVersions.h>

#include <picoquic_packet_loop.h>
#include <picosocks.h>

#include <cstring>
#include <regex>

namespace moxygen::transports {

PicoquicMoQClient::PicoquicMoQClient(
    std::shared_ptr<MoQExecutor> executor,
    Config config)
    : executor_(std::move(executor)), config_(std::move(config)) {}

PicoquicMoQClient::~PicoquicMoQClient() {
  close(SessionCloseErrorCode::NO_ERROR);

  if (eventLoopThread_.joinable()) {
    eventLoopThread_.join();
  }

  if (quic_) {
    picoquic_free(quic_);
    quic_ = nullptr;
  }
}

bool PicoquicMoQClient::initQuicContext() {
  // Create QUIC context
  quic_ = picoquic_create(
      1,  // max_connections
      config_.certFile.empty() ? nullptr : config_.certFile.c_str(),
      config_.keyFile.empty() ? nullptr : config_.keyFile.c_str(),
      config_.caFile.empty() ? nullptr : config_.caFile.c_str(),
      config_.alpn.c_str(),
      nullptr,  // default_callback_fn (set per connection)
      nullptr,  // default_callback_ctx
      nullptr,  // cnx_id_callback
      nullptr,  // cnx_id_callback_data
      nullptr,  // reset_seed
      picoquic_current_time(),
      nullptr,  // simulated_time
      nullptr,  // ticket_file_name
      nullptr,  // ticket_encryption_key
      0);       // ticket_encryption_key_length

  if (!quic_) {
    return false;
  }

  // Configure QUIC settings
  picoquic_set_default_idle_timeout(
      quic_, static_cast<uint64_t>(config_.idleTimeout.count()) * 1000);

  if (config_.insecure) {
    picoquic_set_verify_certificate_callback(
        quic_, nullptr, nullptr);
  }

  return true;
}

#if MOXYGEN_USE_FOLLY
compat::Task<compat::Expected<
    PicoquicMoQClient::ConnectResult,
    PicoquicMoQClient::ConnectError>>
PicoquicMoQClient::connect(
    std::chrono::milliseconds timeout,
    std::shared_ptr<Publisher> publishHandler,
    std::shared_ptr<Subscriber> subscribeHandler) {
  // Folly mode implementation would use coroutines
  // For now, return an error since this is primarily for std-mode
  co_return compat::makeUnexpected(
      SessionCloseErrorCode::INTERNAL_ERROR);
}
#else

void PicoquicMoQClient::connectWithCallback(
    std::chrono::milliseconds timeout,
    std::shared_ptr<Publisher> publishHandler,
    std::shared_ptr<Subscriber> subscribeHandler,
    std::shared_ptr<compat::ResultCallback<ConnectResult, ConnectError>>
        callback) {
  if (!callback) {
    return;
  }

  if (connected_ || session_) {
    callback->onError(SessionCloseErrorCode::PROTOCOL_VIOLATION);
    return;
  }

  // Store callback and handlers for when connection completes
  connectCallback_ = callback;
  pendingPublishHandler_ = publishHandler;
  pendingSubscribeHandler_ = subscribeHandler;

  // Initialize QUIC context
  if (!initQuicContext()) {
    callback->onError(SessionCloseErrorCode::INTERNAL_ERROR);
    connectCallback_.reset();
    return;
  }

  // Resolve server address
  struct sockaddr_storage server_addr;
  int server_addr_len = sizeof(server_addr);

  std::string sni = config_.sni.empty() ? config_.serverHost : config_.sni;

  int ret = picoquic_get_server_address(
      config_.serverHost.c_str(),
      config_.serverPort,
      &server_addr,
      &server_addr_len);

  if (ret != 0) {
    callback->onError(SessionCloseErrorCode::INTERNAL_ERROR);
    connectCallback_.reset();
    return;
  }

  // Create connection
  cnx_ = picoquic_create_cnx(
      quic_,
      picoquic_null_connection_id,
      picoquic_null_connection_id,
      reinterpret_cast<struct sockaddr*>(&server_addr),
      picoquic_current_time(),
      0,  // preferred_version (0 = default)
      sni.c_str(),
      config_.alpn.c_str(),
      1);  // client_mode

  if (!cnx_) {
    callback->onError(SessionCloseErrorCode::INTERNAL_ERROR);
    connectCallback_.reset();
    return;
  }

  // Set callback for this connection
  picoquic_set_callback(cnx_, picoquicCallback, this);

  // Start connection
  ret = picoquic_start_client_cnx(cnx_);
  if (ret != 0) {
    callback->onError(SessionCloseErrorCode::INTERNAL_ERROR);
    connectCallback_.reset();
    picoquic_delete_cnx(cnx_);
    cnx_ = nullptr;
    return;
  }

  // Run the event loop in a separate thread
  // TODO: Use timeout parameter for connection timeout
  eventLoopThread_ = std::thread([this]() {
    runConnection();
  });
}
#endif

void PicoquicMoQClient::runConnection() {
  // Use picoquic's packet loop
  int ret = picoquic_packet_loop(
      quic_,
      0,        // local port (0 = ephemeral)
      0,        // local_af (0 = auto)
      0,        // dest_if
      0,        // socket_buffer_size (0 = default)
      0,        // do_not_use_gso
      nullptr,  // loop_callback
      nullptr); // loop_callback_ctx

  if (ret != 0 && !closed_) {
    onConnectionError(static_cast<uint32_t>(ret));
  }
}

int PicoquicMoQClient::picoquicCallback(
    picoquic_cnx_t* cnx,
    uint64_t stream_id,
    uint8_t* bytes,
    size_t length,
    picoquic_call_back_event_t event,
    void* callback_ctx,
    void* stream_ctx) {
  auto* client = static_cast<PicoquicMoQClient*>(callback_ctx);
  if (!client) {
    return 0;
  }

  switch (event) {
    case picoquic_callback_ready:
      // Connection is ready
      client->onConnected();
      break;

    case picoquic_callback_close:
    case picoquic_callback_application_close:
      client->onConnectionError(0);
      break;

    default:
      // Forward to transport if it exists
      if (client->transport_) {
        return PicoquicWebTransport::picoCallback(
            cnx, stream_id, bytes, length, event, client->transport_.get(), stream_ctx);
      }
      break;
  }

  return 0;
}

void PicoquicMoQClient::onConnected() {
  if (connected_.exchange(true)) {
    return;  // Already connected
  }

  // Create transport wrapper
  transport_ = std::make_shared<PicoquicWebTransport>(cnx_, false);

  // Update connection callback context to use transport directly
  picoquic_set_callback(cnx_, PicoquicWebTransport::picoCallback, transport_.get());

#if !MOXYGEN_USE_FOLLY
  // Create MoQ session (std-mode)
  session_ = std::make_shared<MoQSession>(transport_, executor_);

  // Set up transport callbacks
  transport_->setNewUniStreamCallback([this](compat::StreamReadHandle* stream) {
    if (session_) {
      session_->onNewUniStream(stream);
    }
  });

  transport_->setNewBidiStreamCallback([this](compat::BidiStreamHandle* stream) {
    if (session_) {
      session_->onNewBidiStream(stream);
    }
  });

  transport_->setDatagramCallback(
      [this](std::unique_ptr<compat::Payload> datagram) {
        if (session_) {
          session_->onDatagram(std::move(datagram));
        }
      });

  transport_->setSessionCloseCallback([this](std::optional<uint32_t> error) {
    if (session_) {
      session_->onSessionEnd(error);
    }
  });

  // Set handlers
  if (pendingPublishHandler_) {
    session_->setPublishHandler(pendingPublishHandler_);
    pendingPublishHandler_.reset();
  }
  if (pendingSubscribeHandler_) {
    session_->setSubscribeHandler(pendingSubscribeHandler_);
    pendingSubscribeHandler_.reset();
  }

  // Start the session
  session_->start();

  // Perform MoQ setup handshake
  auto setupCallback = std::make_shared<compat::LambdaResultCallback<
      ServerSetup, SessionCloseErrorCode>>(
      [this](ServerSetup serverSetup) {
        if (connectCallback_) {
          ConnectResult result;
          result.session = session_;
          result.serverSetup = std::move(serverSetup);
          connectCallback_->onSuccess(std::move(result));
          connectCallback_.reset();
        }
      },
      [this](SessionCloseErrorCode error) {
        if (connectCallback_) {
          connectCallback_->onError(error);
          connectCallback_.reset();
        }
      });

  ClientSetup clientSetup;
  clientSetup.supportedVersions = {kVersionDraftCurrent};
  session_->setupWithCallback(std::move(clientSetup), setupCallback);
#endif
}

void PicoquicMoQClient::onConnectionError(uint32_t errorCode) {
  if (closed_.exchange(true)) {
    return;
  }

#if !MOXYGEN_USE_FOLLY
  if (connectCallback_) {
    connectCallback_->onError(SessionCloseErrorCode::INTERNAL_ERROR);
    connectCallback_.reset();
  }
#endif

  if (session_) {
    session_->close(SessionCloseErrorCode::INTERNAL_ERROR);
    session_.reset();
  }
}

std::shared_ptr<MoQSession> PicoquicMoQClient::getSession() const {
  return session_;
}

void PicoquicMoQClient::close(SessionCloseErrorCode errorCode) {
  if (closed_.exchange(true)) {
    return;
  }

  if (session_) {
    session_->close(errorCode);
    session_.reset();
  }

  if (transport_) {
    transport_->closeSession(static_cast<uint32_t>(errorCode));
    transport_.reset();
  }

  // Signal event loop to exit
  if (cnx_) {
    picoquic_close(cnx_, static_cast<uint64_t>(errorCode));
  }
}

// --- PicoquicMoQClientFactory ---

std::unique_ptr<compat::MoQClientInterface> PicoquicMoQClientFactory::createClient(
    std::shared_ptr<MoQExecutor> executor,
    const std::string& url) {
  // Parse URL to extract host and port
  // Expected format: moq://host:port or moq-wt://host:port/path
  PicoquicMoQClient::Config config = defaultConfig_;

  std::regex urlRegex(R"(moq(?:-wt)?://([^:/]+)(?::(\d+))?(?:/.*)?)", std::regex::icase);
  std::smatch match;

  if (std::regex_match(url, match, urlRegex)) {
    config.serverHost = match[1].str();
    if (match[2].matched) {
      config.serverPort = static_cast<uint16_t>(std::stoi(match[2].str()));
    }
  } else {
    // Assume it's just a host:port
    size_t colonPos = url.find(':');
    if (colonPos != std::string::npos) {
      config.serverHost = url.substr(0, colonPos);
      config.serverPort = static_cast<uint16_t>(std::stoi(url.substr(colonPos + 1)));
    } else {
      config.serverHost = url;
    }
  }

  return std::make_unique<PicoquicMoQClient>(std::move(executor), std::move(config));
}

} // namespace moxygen::transports

#endif // MOXYGEN_QUIC_PICOQUIC
