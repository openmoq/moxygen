/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <moxygen/MoQSession.h>
#include <moxygen/MoQTypes.h>
#include <moxygen/compat/Callbacks.h>
#include <moxygen/compat/Config.h>
#include <moxygen/compat/SocketAddress.h>
#include <moxygen/compat/WebTransportInterface.h>
#include <moxygen/events/MoQExecutor.h>

#include <functional>
#include <memory>
#include <string>

namespace moxygen::compat {

/**
 * MoQServerInterface - Abstract interface for MoQ servers
 *
 * This interface decouples MoQ server logic from the transport layer,
 * allowing different transport implementations (Proxygen, libwebsockets, etc.)
 */
class MoQServerInterface {
 public:
  virtual ~MoQServerInterface() = default;

  /**
   * Callback interface for handling new sessions
   */
  class SessionHandler {
   public:
    virtual ~SessionHandler() = default;

    /**
     * Called when a new client session is established
     * @param session The new MoQ session
     * @param clientSetup The client's setup message
     * @return ServerSetup on success, or error code on failure
     */
    virtual Expected<ServerSetup, SessionCloseErrorCode> onNewSession(
        std::shared_ptr<MoQSession> session,
        const ClientSetup& clientSetup) = 0;

    /**
     * Called when a session is terminated
     */
    virtual void onSessionTerminated(std::shared_ptr<MoQSession> session) = 0;
  };

  /**
   * Start listening for connections
   * @param addr The address to listen on
   * @param handler Callback handler for new sessions
   */
  virtual void start(
      const SocketAddress& addr,
      std::shared_ptr<SessionHandler> handler) = 0;

  /**
   * Stop the server
   */
  virtual void stop() = 0;

  /**
   * Check if the server is running
   */
  virtual bool isRunning() const = 0;

  /**
   * Get the executor used by this server
   */
  virtual std::shared_ptr<MoQExecutor> getExecutor() const = 0;
};

/**
 * MoQServerFactory - Factory for creating transport-specific servers
 *
 * Applications implement this to provide their transport layer.
 */
class MoQServerFactory {
 public:
  virtual ~MoQServerFactory() = default;

  /**
   * Create a server with the specified endpoint
   * @param executor The executor for async operations
   * @param endpoint The server endpoint path (e.g., "/moq")
   */
  virtual std::unique_ptr<MoQServerInterface> createServer(
      std::shared_ptr<MoQExecutor> executor,
      const std::string& endpoint) = 0;
};

} // namespace moxygen::compat
