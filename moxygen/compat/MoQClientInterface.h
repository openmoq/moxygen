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
#include <moxygen/compat/WebTransportInterface.h>
#include <moxygen/events/MoQExecutor.h>

#include <functional>
#include <memory>
#include <string>

namespace moxygen::compat {

/**
 * MoQClientInterface - Abstract interface for MoQ clients
 *
 * This interface decouples MoQ client logic from the transport layer,
 * allowing different transport implementations (Proxygen, libwebsockets, etc.)
 */
class MoQClientInterface {
 public:
  virtual ~MoQClientInterface() = default;

  /**
   * Connection result containing the established session
   */
  struct ConnectResult {
    std::shared_ptr<MoQSession> session;
    ServerSetup serverSetup;
  };

  using ConnectError = SessionCloseErrorCode;

#if MOXYGEN_USE_FOLLY && MOXYGEN_QUIC_MVFST
  /**
   * Connect to a MoQ server and establish a session (coroutine version)
   */
  virtual Task<Expected<ConnectResult, ConnectError>> connect(
      std::chrono::milliseconds timeout,
      std::shared_ptr<Publisher> publishHandler,
      std::shared_ptr<Subscriber> subscribeHandler) = 0;
#else
  /**
   * Connect to a MoQ server and establish a session (callback version)
   */
  virtual void connectWithCallback(
      std::chrono::milliseconds timeout,
      std::shared_ptr<Publisher> publishHandler,
      std::shared_ptr<Subscriber> subscribeHandler,
      std::shared_ptr<ResultCallback<ConnectResult, ConnectError>> callback) = 0;
#endif

  /**
   * Get the current session (may be null if not connected)
   */
  virtual std::shared_ptr<MoQSession> getSession() const = 0;

  /**
   * Close the client connection
   */
  virtual void close(SessionCloseErrorCode errorCode = SessionCloseErrorCode::NO_ERROR) = 0;

  /**
   * Get the executor used by this client
   */
  virtual std::shared_ptr<MoQExecutor> getExecutor() const = 0;
};

/**
 * MoQClientFactory - Factory for creating transport-specific clients
 *
 * Applications implement this to provide their transport layer.
 */
class MoQClientFactory {
 public:
  virtual ~MoQClientFactory() = default;

  /**
   * Create a client for connecting to the specified URL
   */
  virtual std::unique_ptr<MoQClientInterface> createClient(
      std::shared_ptr<MoQExecutor> executor,
      const std::string& url) = 0;
};

} // namespace moxygen::compat
