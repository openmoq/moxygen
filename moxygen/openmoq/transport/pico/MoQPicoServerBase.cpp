/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "moxygen/openmoq/transport/pico/MoQPicoServerBase.h"
#include <folly/String.h>
#include <folly/logging/xlog.h>
#include <moxygen/MoQFramer.h>
#include <moxygen/MoQSession.h>
#include <moxygen/MoQVersions.h>
#include <moxygen/openmoq/transport/pico/PicoConnectionContext.h>
#include <moxygen/openmoq/transport/pico/PicoH3WebTransport.h>
#include <moxygen/openmoq/transport/pico/PicoProtocolDispatcher.h>
#include <picoquic.h>
#include <picoquic_bbr.h>
#include <h3zero_common.h>
#include <pico_webtransport.h>

namespace moxygen {

// Bridge struct declared as friend in MoQPicoServerBase. Allows the
// anonymous-namespace callbacks to call private methods.
struct MoQPicoServerBaseCallbacks {
  static void onNewConnection(MoQPicoServerBase* server, void* cnx) {
    server->onNewConnectionImpl(cnx);
  }
  static void onNewWebTransportConnection(MoQPicoServerBase* server, void* cnx) {
    server->onNewWebTransportConnectionImpl(cnx);
  }
  static const std::string& getVersions(MoQPicoServerBase* server) {
    return server->versions_;
  }
  static const PicoWebTransportConfig& getWTConfig(MoQPicoServerBase* server) {
    return server->wtConfig_;
  }
  static h3zero_callback_ctx_t* getH3Ctx(MoQPicoServerBase* server) {
    return server->h3Ctx_;
  }
  static int onWebTransportConnect(
      MoQPicoServerBase* server,
      picoquic_cnx_t* cnx,
      h3zero_stream_ctx_t* streamCtx) {
    return server->onWebTransportConnectImpl(cnx, streamCtx);
  }
  static int onWebTransportEvent(
      MoQPicoServerBase* server,
      picoquic_cnx_t* cnx,
      uint8_t* bytes,
      size_t length,
      int event,
      h3zero_stream_ctx_t* streamCtx) {
    return server->onWebTransportEventImpl(cnx, bytes, length, event, streamCtx);
  }
  static std::shared_ptr<MoQExecutor> getExecutor(MoQPicoServerBase* server) {
    return server->executor_;
  }
};

namespace {

static size_t alpnSelectCallback(picoquic_quic_t* quic,
                                 picoquic_iovec_t* list,
                                 size_t count) {
  auto* server = static_cast<MoQPicoServerBase*>(
      picoquic_get_default_callback_context(quic));
  const auto& wtConfig = MoQPicoServerBaseCallbacks::getWTConfig(server);

  // Build list of supported ALPNs based on configuration
  std::vector<std::string> supportedAlpns;

  // Add h3 first if WebTransport is enabled (higher priority for browsers)
  if (wtConfig.enableWebTransport) {
    supportedAlpns.push_back("h3");
  }

  // Add raw MoQ ALPNs if enabled
  if (wtConfig.enableRawMoQ) {
    auto moqtAlpns =
        getMoqtProtocols(MoQPicoServerBaseCallbacks::getVersions(server), true);
    supportedAlpns.insert(
        supportedAlpns.end(), moqtAlpns.begin(), moqtAlpns.end());
  }

  std::vector<std::string> clientAlpns;
  for (size_t i = 0; i < count; i++) {
    if (list[i].base && list[i].len > 0) {
      clientAlpns.emplace_back(reinterpret_cast<const char*>(list[i].base),
                               list[i].len);
    }
  }
  XLOG(DBG4) << "Client proposed ALPNs: " << folly::join(", ", clientAlpns);

  for (const auto& ourAlpn : supportedAlpns) {
    for (size_t i = 0; i < clientAlpns.size(); i++) {
      if (clientAlpns[i] == ourAlpn) {
        XLOG(DBG1) << "Selected ALPN: " << ourAlpn << " (index " << i << ")";
        return i;
      }
    }
  }

  XLOG(WARN) << "No common ALPN found between client and server";
  return count;
}

static int picoCallback(picoquic_cnx_t* cnx,
                        uint64_t stream_id,
                        uint8_t* bytes,
                        size_t length,
                        picoquic_call_back_event_t fin_or_event,
                        void* callback_ctx,
                        void* v_stream_ctx) {
  XLOG(DBG6) << "picoCallback: event=" << fin_or_event
             << " stream_id=" << stream_id << " length=" << length;

  if (fin_or_event == picoquic_callback_ready ||
      fin_or_event == picoquic_callback_almost_ready ||
      fin_or_event == picoquic_callback_request_alpn_list ||
      fin_or_event == picoquic_callback_set_alpn) {
    auto* server = static_cast<MoQPicoServerBase*>(callback_ctx);
    if (!server) {
      XLOG(ERR) << "picoCallback: server is null on event " << fin_or_event;
      return PICOQUIC_ERROR_UNEXPECTED_ERROR;
    }
    if (fin_or_event == picoquic_callback_ready) {
      XLOG(DBG1) << "New connection ready";

      // Determine protocol based on negotiated ALPN
      const char* alpn = picoquic_tls_get_negotiated_alpn(cnx);
      auto protocol = PicoProtocolDispatcher::getProtocol(alpn);

      XLOG(DBG1) << "Negotiated ALPN: " << (alpn ? alpn : "(null)")
                 << " -> Protocol: "
                 << PicoProtocolDispatcher::protocolName(protocol);

      if (protocol == PicoProtocolType::WebTransport) {
        // Route to h3zero/WebTransport handler
        MoQPicoServerBaseCallbacks::onNewWebTransportConnection(server, cnx);
      } else {
        // Route to raw MoQ handler (existing path)
        MoQPicoServerBaseCallbacks::onNewConnection(server, cnx);
      }
    } else {
      XLOG(DBG2) << "Connection event: " << fin_or_event;
    }
    return 0;
  }

  auto* ctx = static_cast<PicoConnectionContext*>(callback_ctx);
  if (!ctx) {
    return 0;
  }
  if (ctx->magic != PicoConnectionContext::kMagic) {
    XLOG(DBG1) << "Connection closing before context created, event="
               << fin_or_event;
    return 0;
  }

  XLOG(DBG6) << "Forwarding event " << fin_or_event << " to PicoQuicWebTransport";
  return dispatchConnectionEvent(
      ctx, cnx, stream_id, bytes, length, fin_or_event, v_stream_ctx);
}

// WebTransport path callback - invoked by h3zero when a CONNECT request
// is received on the configured endpoint (e.g., /moq)
static int wtPathCallback(
    picoquic_cnx_t* cnx,
    uint8_t* bytes,
    size_t length,
    picohttp_call_back_event_t event,
    h3zero_stream_ctx_t* streamCtx,
    void* pathAppCtx) {
  auto* server = static_cast<MoQPicoServerBase*>(pathAppCtx);
  if (!server) {
    XLOG(ERR) << "wtPathCallback: server is null";
    return -1;
  }

  XLOG(DBG3) << "wtPathCallback: event=" << static_cast<int>(event)
             << " stream=" << (streamCtx ? streamCtx->stream_id : 0)
             << " length=" << length;

  switch (event) {
    case picohttp_callback_connect:
      // Browser sent CONNECT request - accept WebTransport session
      return MoQPicoServerBaseCallbacks::onWebTransportConnect(
          server, cnx, streamCtx);

    case picohttp_callback_connect_refused:
      XLOG(WARN) << "WebTransport CONNECT refused";
      break;

    case picohttp_callback_connect_accepted:
      XLOG(DBG1) << "WebTransport CONNECT accepted";
      break;

    default:
      // Forward other events to the WebTransport adapter
      return MoQPicoServerBaseCallbacks::onWebTransportEvent(
          server, cnx, bytes, length, static_cast<int>(event), streamCtx);
  }

  return 0;
}

} // namespace

// ---------------------------------------------------------------------------

MoQPicoServerBase::MoQPicoServerBase(std::string cert,
                                     std::string key,
                                     std::string endpoint,
                                     std::string versions,
                                     PicoWebTransportConfig wtConfig)
    : MoQServerBase(std::move(endpoint)),
      cert_(std::move(cert)),
      key_(std::move(key)),
      versions_(std::move(versions)),
      wtConfig_(std::move(wtConfig)) {}

MoQPicoServerBase::~MoQPicoServerBase() {
  destroyH3Zero();
  destroyQuicContext();
}

bool MoQPicoServerBase::createQuicContext() {
  // Build and log supported ALPNs
  std::vector<std::string> supportedAlpns;
  if (wtConfig_.enableWebTransport) {
    supportedAlpns.push_back("h3");
  }
  if (wtConfig_.enableRawMoQ) {
    auto moqtAlpns = getMoqtProtocols(versions_, true);
    supportedAlpns.insert(
        supportedAlpns.end(), moqtAlpns.begin(), moqtAlpns.end());
  }
  XLOG(INFO) << "Supported ALPNs: " << folly::join(", ", supportedAlpns);

  uint64_t current_time = picoquic_current_time();
  quic_ = picoquic_create(
      100,
      cert_.c_str(),
      key_.c_str(),
      nullptr, // cert_store_filename
      nullptr, // default_alpn (NULL — use ALPN selection callback)
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
    XLOG(ERR) << "Failed to create picoquic context (cert=" << cert_
              << ", key=" << key_ << ")";
    return false;
  }

  picoquic_set_alpn_select_fn_v2(quic_, alpnSelectCallback);
  picoquic_set_cookie_mode(quic_, 2);
  picoquic_set_default_congestion_algorithm(quic_, picoquic_bbr_algorithm);

  // Initialize h3zero for WebTransport if enabled
  if (wtConfig_.enableWebTransport) {
    if (!initH3Zero()) {
      XLOG(ERR) << "Failed to initialize h3zero for WebTransport";
      destroyQuicContext();
      return false;
    }
    // Enable WebTransport transport parameters
    picowt_set_default_transport_parameters(quic_);
    XLOG(INFO) << "WebTransport enabled on endpoint: " << wtConfig_.wtEndpoint;
  }

  return true;
}

void MoQPicoServerBase::destroyQuicContext() {
  if (quic_) {
    // picoquic_free will close all connections, which will trigger close
    // callbacks
    picoquic_free(quic_);
    quic_ = nullptr;
  }
}

void MoQPicoServerBase::onNewConnectionImpl(void* vcnx) {
  auto* cnx = static_cast<picoquic_cnx_t*>(vcnx);

  struct sockaddr* local_addr_ptr = nullptr;
  struct sockaddr* peer_addr_ptr = nullptr;
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

  auto webTransport =
      std::make_shared<PicoQuicWebTransport>(cnx, localSockAddr, peerSockAddr);
  onWebTransportCreated(*webTransport);

  auto moqSession = createSession(webTransport, executor_);
  webTransport->setHandler(moqSession.get());

  const char* alpn = picoquic_tls_get_negotiated_alpn(cnx);
  if (alpn) {
    XLOG(DBG1) << "Setting MoQ version from negotiated ALPN: " << alpn;
    moqSession->validateAndSetVersionFromAlpn(alpn);
  } else {
    XLOG(WARN) << "No ALPN was negotiated for connection";
  }

  auto* ctx = new PicoConnectionContext{
      .webTransport = webTransport, .moqSession = moqSession};

  XLOG(DBG4) << "Setting connection callback context";
  picoquic_set_callback(cnx, picoCallback, ctx);

  folly::coro::co_withExecutor(executor_.get(), handleClientSession(moqSession))
      .start();
}

void MoQPicoServerBase::onNewWebTransportConnectionImpl(void* vcnx) {
  auto* cnx = static_cast<picoquic_cnx_t*>(vcnx);

  struct sockaddr* peer_addr_ptr = nullptr;
  picoquic_get_peer_addr(cnx, &peer_addr_ptr);

  folly::SocketAddress peerSockAddr;
  if (peer_addr_ptr) {
    peerSockAddr.setFromSockaddr(peer_addr_ptr);
  }

  XLOG(DBG1) << "New WebTransport connection from " << peerSockAddr.describe();

  if (!h3Ctx_) {
    XLOG(ERR) << "h3zero context not initialized for WebTransport connection";
    picoquic_close(cnx, H3ZERO_INTERNAL_ERROR);
    return;
  }

  // Initialize HTTP/3 protocol for this connection
  int ret = h3zero_protocol_init(cnx);
  if (ret != 0) {
    XLOG(ERR) << "Failed to initialize h3zero protocol for connection: " << ret;
    picoquic_close(cnx, H3ZERO_INTERNAL_ERROR);
    return;
  }

  // Set h3zero callback for this connection
  // The h3zero callback will handle HTTP/3 framing and WebTransport
  picoquic_set_callback(cnx, h3zero_callback, h3Ctx_);

  // Enable WebTransport transport parameters for this connection
  picowt_set_transport_parameters(cnx);

  XLOG(DBG1) << "WebTransport connection initialized, waiting for CONNECT";

  // Note: MoQSession will be created when WebTransport CONNECT is received
  // This is handled in the WebTransport path callback (wtPathCallback)
}

bool MoQPicoServerBase::initH3Zero() {
  // Create path table for WebTransport endpoint
  // We need to keep this alive for the lifetime of the server
  wtPathTable_ = std::make_unique<picohttp_server_path_item_t[]>(2);

  // Configure the WebTransport endpoint path
  // The callback will be invoked when a CONNECT request is received
  wtPathTable_[0].path = wtConfig_.wtEndpoint.c_str();
  wtPathTable_[0].path_length = wtConfig_.wtEndpoint.size();
  wtPathTable_[0].path_callback = wtPathCallback;
  wtPathTable_[0].path_app_ctx = this;

  // Null terminator for path table
  wtPathTable_[1].path = nullptr;
  wtPathTable_[1].path_length = 0;
  wtPathTable_[1].path_callback = nullptr;
  wtPathTable_[1].path_app_ctx = nullptr;

  // Create h3zero server parameters
  picohttp_server_parameters_t serverParams = {};
  serverParams.web_folder = nullptr;  // No static file serving
  serverParams.path_table = wtPathTable_.get();
  serverParams.path_table_nb = 1;

  // Create h3zero callback context
  h3Ctx_ = h3zero_callback_create_context(&serverParams);
  if (!h3Ctx_) {
    XLOG(ERR) << "Failed to create h3zero callback context";
    return false;
  }

  // Configure h3zero settings for WebTransport
  h3Ctx_->settings.h3_datagram = 1;  // Enable datagrams
  h3Ctx_->settings.webtransport_max_sessions = wtConfig_.wtMaxSessions;

  XLOG(DBG1) << "h3zero initialized with WebTransport endpoint: "
             << wtConfig_.wtEndpoint;

  return true;
}

void MoQPicoServerBase::destroyH3Zero() {
  if (h3Ctx_) {
    h3zero_callback_delete_context(nullptr, h3Ctx_);
    h3Ctx_ = nullptr;
  }
  wtPathTable_.reset();
}

int MoQPicoServerBase::onWebTransportConnectImpl(
    picoquic_cnx_t* cnx,
    h3zero_stream_ctx_t* streamCtx) {
  XLOG(DBG1) << "WebTransport CONNECT received on stream " << streamCtx->stream_id;

  // Get addresses
  struct sockaddr* local_addr_ptr = nullptr;
  struct sockaddr* peer_addr_ptr = nullptr;
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

  XLOG(DBG1) << "Accepting WebTransport session from " << peerSockAddr.describe();

  // Create PicoH3WebTransport adapter
  auto webTransport = std::make_shared<PicoH3WebTransport>(
      cnx, h3Ctx_, streamCtx, localSockAddr, peerSockAddr);

  // Create MoQSession
  auto moqSession = createSession(webTransport, executor_);
  webTransport->setHandler(moqSession.get());

  // For WebTransport, MoQ version is always the latest (client indicates via WT)
  // TODO: Version negotiation via WebTransport headers if needed

  // Store the context for routing future events
  // Use the control stream's path_callback_ctx to store our WebTransport adapter
  streamCtx->path_callback_ctx = webTransport.get();

  // Store in wtSessions_ map for lifecycle management
  wtSessions_[streamCtx->stream_id] = {webTransport, moqSession};

  // Notify subclass
  onNewSession(moqSession);

  // Send 200 OK response to accept the WebTransport session
  uint8_t response[256];
  uint8_t* bytes = response;
  uint8_t* bytes_max = response + sizeof(response);

  // Create response header with 200 OK status
  bytes = h3zero_create_response_header_frame_ex(
      bytes, bytes_max, h3zero_content_type_none, nullptr, nullptr);

  if (bytes == nullptr) {
    XLOG(ERR) << "Failed to create WebTransport response header";
    return -1;
  }

  // Send response
  int ret = picoquic_add_to_stream_with_ctx(
      cnx, streamCtx->stream_id, response, bytes - response, 0, streamCtx);
  if (ret != 0) {
    XLOG(ERR) << "Failed to send WebTransport response: " << ret;
    return ret;
  }

  // Mark stream as upgraded for WebTransport
  streamCtx->is_upgraded = 1;

  // Start handling the MoQ session
  folly::coro::co_withExecutor(executor_.get(), handleClientSession(moqSession))
      .start();

  XLOG(DBG1) << "WebTransport session accepted on stream " << streamCtx->stream_id;
  return 0;
}

int MoQPicoServerBase::onWebTransportEventImpl(
    picoquic_cnx_t* cnx,
    uint8_t* bytes,
    size_t length,
    int event,
    h3zero_stream_ctx_t* streamCtx) {
  // Find the WebTransport adapter for this stream
  PicoH3WebTransport* wt = nullptr;

  // Check if this is the control stream or a data stream
  if (streamCtx && streamCtx->path_callback_ctx) {
    wt = static_cast<PicoH3WebTransport*>(streamCtx->path_callback_ctx);
  } else if (streamCtx) {
    // For data streams, find the session by control stream ID
    uint64_t controlStreamId = streamCtx->ps.stream_state.control_stream_id;
    auto it = wtSessions_.find(controlStreamId);
    if (it != wtSessions_.end()) {
      wt = it->second.webTransport.get();
    }
  }

  if (!wt) {
    XLOG(DBG2) << "No WebTransport adapter found for event "
               << static_cast<int>(event);
    return 0;
  }

  return wt->handleWtEvent(cnx, bytes, length, static_cast<int>(event), streamCtx);
}

} // namespace moxygen
