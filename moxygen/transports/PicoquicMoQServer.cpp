/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "moxygen/transports/PicoquicMoQServer.h"

#if MOXYGEN_QUIC_PICOQUIC

#include <moxygen/MoQSession.h>
#include <moxygen/MoQTypes.h>
#include <moxygen/MoQVersions.h>
#include <moxygen/compat/Try.h>

#include <picoquic_packet_loop.h>
#include <picosocks.h>

#include <cstdio>
#include <cstring>

namespace moxygen::transports {

#if !MOXYGEN_USE_FOLLY
// Server-side setup callback implementation
class PicoquicMoQServer::ServerSetupCallbackImpl
    : public MoQSession::ServerSetupCallback {
 public:
  explicit ServerSetupCallbackImpl(
      std::shared_ptr<compat::MoQServerInterface::SessionHandler> handler)
      : handler_(std::move(handler)) {}

  compat::Try<ServerSetup> onClientSetup(
      ClientSetup clientSetup,
      const std::shared_ptr<MoQSessionBase>& session) override {
    if (handler_) {
      // Cast to MoQSession for the handler
      auto moqSession = std::dynamic_pointer_cast<MoQSession>(
          std::const_pointer_cast<MoQSessionBase>(session));
      if (moqSession) {
        auto result = handler_->onNewSession(moqSession, clientSetup);
        if (result.hasValue()) {
          return compat::Try<ServerSetup>(std::move(result.value()));
        }
        return compat::Try<ServerSetup>(
            std::make_exception_ptr(std::runtime_error("Session rejected")));
      }
    }
    // Default setup
    ServerSetup setup;
    setup.selectedVersion = kVersionDraftCurrent;
    return compat::Try<ServerSetup>(std::move(setup));
  }

  compat::Expected<compat::Unit, SessionCloseErrorCode> validateAuthority(
      const ClientSetup& /*clientSetup*/,
      uint64_t /*negotiatedVersion*/,
      std::shared_ptr<MoQSessionBase> /*session*/) override {
    return compat::Unit{};
  }

 private:
  std::shared_ptr<compat::MoQServerInterface::SessionHandler> handler_;
};
#endif

PicoquicMoQServer::PicoquicMoQServer(
    std::shared_ptr<MoQExecutor> executor,
    Config config,
    std::string endpoint)
    : executor_(std::move(executor)),
      config_(std::move(config)),
      endpoint_(std::move(endpoint)) {}

PicoquicMoQServer::~PicoquicMoQServer() {
  stop();

  // Safe to access after pico thread is stopped
  connections_.clear();

  if (quic_) {
    picoquic_free(quic_);
    quic_ = nullptr;
  }
}

bool PicoquicMoQServer::initQuicContext() {
  // Create QUIC context
  quic_ = picoquic_create(
      static_cast<uint32_t>(config_.maxConnections),
      config_.certFile.c_str(),
      config_.keyFile.c_str(),
      nullptr,  // cert_root_file (not needed for server)
      config_.alpn.c_str(),
      nullptr,  // default_callback_fn (set separately)
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

  // Set server-side callback
  picoquic_set_default_callback(quic_, picoquicCallback, this);

  // Configure QUIC settings
  picoquic_set_default_idle_timeout(
      quic_, static_cast<uint64_t>(config_.idleTimeout.count()) * 1000);

  return true;
}

void PicoquicMoQServer::start(
    const compat::SocketAddress& addr,
    std::shared_ptr<SessionHandler> handler) {
  if (running_.exchange(true)) {
    return;  // Already running
  }

  sessionHandler_ = std::move(handler);

  if (!initQuicContext()) {
    running_ = false;
    return;
  }

  // Configure loop parameters
  memset(&loopParam_, 0, sizeof(loopParam_));
  loopParam_.local_port = addr.getPort();

  // Start network thread with loop callback
  int ret = 0;
  threadCtx_ = picoquic_start_network_thread(
      quic_, &loopParam_, picoLoopCallback, this, &ret);

  if (!threadCtx_ || ret != 0) {
    running_ = false;
    return;
  }

  // Wait for thread to be ready
  while (!threadCtx_->thread_is_ready) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  dispatcher_.setThreadContext(threadCtx_);
}

void PicoquicMoQServer::stop() {
  if (!running_.exchange(false)) {
    return;  // Not running
  }

  if (threadCtx_) {
    // Delete the network thread (signals close and waits for thread exit)
    picoquic_delete_network_thread(threadCtx_);
    threadCtx_ = nullptr;
  }
}

int PicoquicMoQServer::picoLoopCallback(
    picoquic_quic_t* /*quic*/,
    picoquic_packet_loop_cb_enum cb_mode,
    void* callback_ctx,
    void* /*callback_argv*/) {
  auto* server = static_cast<PicoquicMoQServer*>(callback_ctx);
  if (!server) {
    return 0;
  }

  switch (cb_mode) {
    case picoquic_packet_loop_ready:
      server->dispatcher_.setPicoThreadId(std::this_thread::get_id());
      break;
    case picoquic_packet_loop_wake_up:
      server->dispatcher_.drainWorkQueue();
      break;
    default:
      break;
  }

  return 0;
}

int PicoquicMoQServer::picoquicCallback(
    picoquic_cnx_t* cnx,
    uint64_t stream_id,
    uint8_t* bytes,
    size_t length,
    picoquic_call_back_event_t event,
    void* callback_ctx,
    void* stream_ctx) {
  auto* server = static_cast<PicoquicMoQServer*>(callback_ctx);
  if (!server) {
    return 0;
  }

  switch (event) {
    case picoquic_callback_ready:
      // New connection is ready
      server->onNewConnection(cnx);
      break;

    case picoquic_callback_close:
    case picoquic_callback_application_close:
      server->onConnectionClose(cnx);
      break;

    default: {
      // Forward to the connection's transport (safe: pico thread only)
      auto it = server->connections_.find(cnx);
      if (it != server->connections_.end() && it->second.transport) {
        return PicoquicWebTransport::picoCallback(
            cnx, stream_id, bytes, length, event,
            it->second.transport.get(), stream_ctx);
      }
      break;
    }
  }

  return 0;
}

void PicoquicMoQServer::onNewConnection(picoquic_cnx_t* cnx) {
  // Create transport wrapper with thread dispatcher
  auto transport = std::make_shared<PicoquicWebTransport>(cnx, true, &dispatcher_);

#if !MOXYGEN_USE_FOLLY
  // Create the setup callback wrapper
  auto setupCallback = std::make_shared<ServerSetupCallbackImpl>(sessionHandler_);

  // Create session with setup callback
  auto session = std::make_shared<MoQSession>(transport, *setupCallback, executor_);

  // Store connection state (safe: pico thread only)
  ConnectionState state;
  state.transport = transport;
  state.session = session;
  state.setupCallback = setupCallback;
  connections_[cnx] = std::move(state);

  // Set up transport callbacks
  transport->setNewUniStreamCallback([session](compat::StreamReadHandle* stream) {
    session->onNewUniStream(stream);
  });

  transport->setNewBidiStreamCallback([session](compat::BidiStreamHandle* stream) {
    session->onNewBidiStream(stream);
  });

  transport->setDatagramCallback(
      [session](std::unique_ptr<compat::Payload> datagram) {
        session->onDatagram(std::move(datagram));
      });

  transport->setSessionCloseCallback([session, this, cnx](std::optional<uint32_t> error) {
    session->onSessionEnd(error);
    onConnectionClose(cnx);
  });

  // Update connection callback to use transport
  picoquic_set_callback(cnx, PicoquicWebTransport::picoCallback, transport.get());

  // Start the session
  session->start();
#endif
}

void PicoquicMoQServer::onConnectionClose(picoquic_cnx_t* cnx) {
  std::shared_ptr<MoQSession> session;

  // Safe: accessed only from pico thread
  auto it = connections_.find(cnx);
  if (it != connections_.end()) {
    session = it->second.session;
    connections_.erase(it);
  }

  if (session && sessionHandler_) {
    sessionHandler_->onSessionTerminated(session);
  }
}

// --- PicoquicMoQServerFactory ---

std::unique_ptr<compat::MoQServerInterface> PicoquicMoQServerFactory::createServer(
    std::shared_ptr<MoQExecutor> executor,
    const std::string& endpoint) {
  return std::make_unique<PicoquicMoQServer>(
      std::move(executor), config_, endpoint);
}

} // namespace moxygen::transports

#endif // MOXYGEN_QUIC_PICOQUIC
