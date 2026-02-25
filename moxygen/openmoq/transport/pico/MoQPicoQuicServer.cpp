/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "moxygen/openmoq/transport/pico/MoQPicoQuicServer.h"
#include <folly/String.h>
#include <folly/experimental/coro/BlockingWait.h>
#include <folly/logging/xlog.h>
#include <moxygen/MoQFramer.h> // For getDefaultMoqtProtocols
#include <moxygen/openmoq/transport/pico/PicoQuicExecutor.h>
#include <moxygen/openmoq/transport/pico/PicoQuicWebTransport.h>
#include <picoquic_bbr.h>
#include <picoquic_packet_loop.h>

namespace moxygen {

// Per-connection context stored in picoquic's callback_ctx
struct ConnectionContext {
  // Magic value to identify this as a ConnectionContext vs server pointer
  static constexpr uint32_t kMagic = 0xC099EC71; // "CONNECT1"
  uint32_t magic{kMagic};

  std::shared_ptr<proxygen::WebTransport> webTransport;
  std::shared_ptr<MoQSession> moqSession;
  MoQPicoQuicServer *server;
};

// Static ALPN selection callback for picoquic
static size_t alpnSelectCallback(picoquic_quic_t *quic, picoquic_iovec_t *list,
                                 size_t count) {
  // Get list of supported MoQT ALPNs (in preference order)
  auto supportedAlpns = getDefaultMoqtProtocols(true);

  // First pass: build list of client-proposed ALPNs
  std::vector<std::string> clientAlpns;
  for (size_t i = 0; i < count; i++) {
    if (list[i].base && list[i].len > 0) {
      clientAlpns.emplace_back(reinterpret_cast<const char *>(list[i].base),
                               list[i].len);
    }
  }
  XLOG(DBG4) << "Client proposed ALPNs: " << folly::join(", ", clientAlpns);

  // Second pass: find the first ALPN from our preference list that the client
  // supports
  for (const auto &ourAlpn : supportedAlpns) {
    for (size_t i = 0; i < clientAlpns.size(); i++) {
      if (clientAlpns[i] == ourAlpn) {
        XLOG(DBG1) << "Selected ALPN: " << ourAlpn << " (index " << i << ")";
        return i;
      }
    }
  }

  XLOG(WARN) << "No common ALPN found between client and server";
  return count; // Return invalid index
}

MoQPicoQuicServer::MoQPicoQuicServer(std::string cert, std::string key,
                                     std::string endpoint)
    : MoQServerBase(std::move(endpoint)), cert_(std::move(cert)),
      key_(std::move(key)) {}

MoQPicoQuicServer::~MoQPicoQuicServer() { stop(); }

void MoQPicoQuicServer::start(const folly::SocketAddress &addr) {
  if (running_.exchange(true)) {
    XLOG(WARN) << "Server already running";
    return;
  }

  serverAddr_ = addr;

  // Create the executor that will be shared by all sessions
  executor_ = std::make_shared<PicoQuicExecutor>();

  // Create the QUIC context
  uint64_t current_time = picoquic_current_time();

  // Get list of supported MoQT ALPNs (in preference order)
  auto supportedAlpns = getDefaultMoqtProtocols(true);

  XLOG(INFO) << "Supported ALPNs: " << folly::join(", ", supportedAlpns);

  // Pass NULL as default ALPN - we'll use the selection callback instead
  quic_ =
      picoquic_create(100, // max_connections
                      cert_.c_str(), key_.c_str(),
                      nullptr, // cert_store_filename
                      nullptr, // default_alpn (NULL to use selection callback)
                      picoCallback,
                      this,    // callback_ctx
                      nullptr, // cnx_id_callback
                      nullptr, // cnx_id_callback_ctx
                      nullptr, // reset_seed
                      current_time,
                      nullptr, // simulated_time
                      nullptr, // ticket_file_name
                      nullptr, // ticket_encryption_key
                      0);      // ticket_encryption_key_length

  if (quic_ == nullptr) {
    XLOG(ERR)
        << "Failed to create picoquic context (check cert/key paths: cert="
        << cert_ << ", key=" << key_ << ")";
    executor_.reset();
    running_ = false;
    return;
  }

  // Set ALPN selection callback to handle multiple ALPNs
  picoquic_set_alpn_select_fn_v2(quic_, alpnSelectCallback);

  // Configure picoquic settings
  picoquic_set_cookie_mode(quic_, 2);
  picoquic_set_default_congestion_algorithm(quic_, picoquic_bbr_algorithm);

  XLOG(INFO) << "Starting MoQPicoQuicServer on " << serverAddr_.describe()
             << " with " << supportedAlpns.size() << " supported ALPNs";

  // Set up packet loop parameters
  loopParam_ = {0};
  loopParam_.local_port = static_cast<uint16_t>(serverAddr_.getPort());
  loopParam_.local_af = 0; // 0 = any
  loopParam_.dest_if = 0;
  loopParam_.socket_buffer_size = 0; // 0 = default
  loopParam_.do_not_use_gso = 0;

  // Start the network thread using picoquic's network thread API
  int ret = 0;
  networkThreadCtx_ = picoquic_start_network_thread(
      quic_, &loopParam_, PicoQuicExecutor::getLoopCallback(), executor_.get(),
      &ret);

  if (networkThreadCtx_ == nullptr) {
    XLOG(ERR) << "Failed to start network thread, ret=" << ret;
    picoquic_free(quic_);
    quic_ = nullptr;
    running_ = false;
    return;
  }

  XLOG(INFO) << "MoQPicoQuicServer network thread started";
}

void MoQPicoQuicServer::stop() {
  if (!running_.exchange(false)) {
    return;
  }

  XLOG(INFO) << "Stopping MoQPicoQuicServer";

  // Stop the network thread cleanly
  if (networkThreadCtx_) {
    picoquic_delete_network_thread(networkThreadCtx_);
    networkThreadCtx_ = nullptr;
  }

  // Free the picoquic context
  // Note: picoquic_free will close all connections, which will trigger close
  // callbacks
  if (quic_) {
    picoquic_free(quic_);
    quic_ = nullptr;
  }

  XLOG(INFO) << "MoQPicoQuicServer stopped";
}

int MoQPicoQuicServer::picoCallback(picoquic_cnx_t *cnx, uint64_t stream_id,
                                    uint8_t *bytes, size_t length,
                                    picoquic_call_back_event_t fin_or_event,
                                    void *callback_ctx, void *v_stream_ctx) {

  // Log ALL callbacks to debug missing stream data
  XLOG(DBG6) << "MoQPicoQuicServer::picoCallback: event=" << fin_or_event
             << " stream_id=" << stream_id << " length=" << length
             << " callback_ctx=" << callback_ctx;

  // For the ready/almost_ready/ALPN events, callback_ctx is the server
  // For all other events, it's the ConnectionContext
  if (fin_or_event == picoquic_callback_ready ||
      fin_or_event == picoquic_callback_almost_ready ||
      fin_or_event == picoquic_callback_request_alpn_list ||
      fin_or_event == picoquic_callback_set_alpn) {
    auto *server = static_cast<MoQPicoQuicServer *>(callback_ctx);
    if (!server) {
      XLOG(ERR) << "picoCallback: server is null on event " << fin_or_event
                << "! This will return "
                << "PICOQUIC_ERROR_UNEXPECTED_ERROR (1051)";
      return PICOQUIC_ERROR_UNEXPECTED_ERROR;
    }

    // Only initialize on ready, not almost_ready or ALPN events
    if (fin_or_event == picoquic_callback_ready) {
      XLOG(DBG1) << "New connection ready";
      server->onNewConnection(cnx);
    } else {
      XLOG(DBG2) << "Connection event: " << fin_or_event;
    }
    return 0;
  }

  // For all other events, get the ConnectionContext
  auto *ctx = static_cast<ConnectionContext *>(callback_ctx);
  if (!ctx) {
    // Connection might be closing before we set context
    return 0;
  }

  // Verify this is actually a ConnectionContext, not the server pointer
  // (connection can close before we create the ConnectionContext)
  if (ctx->magic != ConnectionContext::kMagic) {
    XLOG(DBG1) << "Connection closing before ConnectionContext "
               << "created, event=" << fin_or_event;
    return 0;
  }

  switch (fin_or_event) {
  case picoquic_callback_close:
  case picoquic_callback_application_close:
  case picoquic_callback_stateless_reset: {
    // Connection closed - clean up context
    XLOG(DBG1) << "Connection closed, event=" << fin_or_event;
    if (ctx->moqSession) {
      ctx->moqSession->onSessionEnd(folly::none);
    }
    // Clear magic before deletion to catch use-after-free
    ctx->magic = 0xDEADBEEF;
    delete ctx; // Free the connection context
    return 0;
  }

  default: {
    // Forward all stream events to PicoQuicWebTransport callback
    XLOG(DBG6) << "Forwarding event " << fin_or_event
               << " to PicoQuicWebTransport"
               << " webTransport=" << (void *)ctx->webTransport.get();
    if (ctx->webTransport) {
      return PicoQuicWebTransport::picoCallback(
          cnx, stream_id, bytes, length, fin_or_event, ctx->webTransport.get(),
          v_stream_ctx);
    }
    XLOG(WARN) << "webTransport is null, cannot forward event " << fin_or_event;
    return 0;
  }
  }
}

void MoQPicoQuicServer::onNewConnection(picoquic_cnx_t *cnx) {
  // Get local and peer addresses
  struct sockaddr *local_addr_ptr = nullptr;
  struct sockaddr *peer_addr_ptr = nullptr;

  picoquic_get_peer_addr(cnx, &peer_addr_ptr);
  picoquic_get_local_addr(cnx, &local_addr_ptr);

  folly::SocketAddress localSockAddr;
  folly::SocketAddress peerSockAddr;

  if (local_addr_ptr) {
    localSockAddr.setFromSockaddr(local_addr_ptr);
  }
  if (peer_addr_ptr) {
    peerSockAddr.setFromSockaddr(peer_addr_ptr);
  }

  XLOG(DBG1) << "New connection from " << peerSockAddr.describe();

  // Create PicoQuicWebTransport
  auto webTransport =
      std::make_shared<PicoQuicWebTransport>(cnx, localSockAddr, peerSockAddr);

  // Create MoQSession using the shared executor
  // Note: createSession takes shared_ptr by value, so it copies
  auto moqSession = createSession(webTransport, executor_);

  // Set the WebTransport handler to the MoQSession
  webTransport->setHandler(moqSession.get());

  // Get the negotiated ALPN from picoquic
  const char *alpn = picoquic_tls_get_negotiated_alpn(cnx);
  if (alpn) {
    XLOG(DBG1) << "Setting MoQ version from negotiated ALPN: " << alpn;
    moqSession->validateAndSetVersionFromAlpn(alpn);
  } else {
    XLOG(WARN) << "No ALPN was negotiated for connection";
  }

  // Create connection context and set it as the callback context
  auto *ctx = new ConnectionContext{
      .webTransport = webTransport, .moqSession = moqSession, .server = this};

  // Update the connection's callback context to our ConnectionContext
  // IMPORTANT: This must happen before any other picoquic events can fire
  XLOG(DBG4) << "Setting connection callback context from server "
             << "to ConnectionContext";
  picoquic_set_callback(cnx, picoCallback, ctx);
  XLOG(DBG4) << "Connection callback context updated successfully";

  // Start handling the session in a coroutine scheduled on the executor
  folly::coro::co_withExecutor(executor_.get(), handleClientSession(moqSession))
      .start();
}

} // namespace moxygen
