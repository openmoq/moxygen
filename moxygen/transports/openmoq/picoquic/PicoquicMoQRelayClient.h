/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <moxygen/compat/Config.h>

#if MOXYGEN_QUIC_PICOQUIC

#include <moxygen/MoQRelaySession.h>
#include <moxygen/Publisher.h>
#include <moxygen/Subscriber.h>
#include <moxygen/compat/Callbacks.h>
#include <moxygen/transports/openmoq/picoquic/PicoquicMoQClient.h>

#include <chrono>
#include <memory>
#include <vector>

namespace moxygen::transports {

/**
 * PicoquicMoQRelayClient - Callback-based relay client for picoquic transport
 *
 * This is the callback-based equivalent of MoQRelayClient, designed for use
 * with picoquic transport. It wraps PicoquicMoQClient and provides relay
 * functionality using the callback-based API of MoQRelaySession.
 *
 * Usage:
 *   auto client = std::make_unique<PicoquicMoQRelayClient>(executor, config);
 *   client->setupWithCallback(publisher, subscriber, timeout,
 *     makeResultCallback(
 *       [&client]() {
 *         client->run(publisher, {TrackNamespace(ns, "/")}, ...);
 *       },
 *       [](auto error) { ... }));
 */
class PicoquicMoQRelayClient {
 public:
  PicoquicMoQRelayClient(
      std::shared_ptr<MoQExecutor> executor,
      PicoquicMoQClient::Config config);

  ~PicoquicMoQRelayClient();

  /**
   * Connect to the relay server and setup the session.
   *
   * @param publisher Publisher handler for incoming subscriptions
   * @param subscriber Subscriber handler for incoming publishes
   * @param connectTimeout Connection timeout
   * @param callback Called on success (Unit) or error (SessionCloseErrorCode)
   */
  void setupWithCallback(
      std::shared_ptr<Publisher> publisher,
      std::shared_ptr<Subscriber> subscriber,
      std::chrono::milliseconds connectTimeout,
      std::shared_ptr<
          compat::ResultCallback<compat::Unit, SessionCloseErrorCode>>
          callback);

  /**
   * Announce namespaces to the relay.
   *
   * This method publishes the given namespaces to the relay server,
   * establishing this client as a publisher for those namespaces.
   *
   * @param publisher The publisher (used for identity, not called back)
   * @param namespaces List of namespaces to publish
   * @param callback Called when all namespaces have been announced
   */
  void run(
      std::shared_ptr<Publisher> publisher,
      std::vector<TrackNamespace> namespaces,
      std::shared_ptr<
          compat::ResultCallback<compat::Unit, PublishNamespaceError>>
          callback);

  /**
   * Get the underlying MoQ session.
   */
  std::shared_ptr<MoQSession> getSession() const;

  /**
   * Get the executor.
   */
  std::shared_ptr<MoQExecutor> getExecutor() const {
    return executor_;
  }

  /**
   * Shutdown the relay client cleanly.
   *
   * This will:
   * - Call publishNamespaceDone() on all announced namespaces
   * - Clear publish/subscribe handlers
   * - Close the session
   */
  void shutdown();

 private:
  std::shared_ptr<MoQExecutor> executor_;
  std::unique_ptr<PicoquicMoQClient> client_;
  std::vector<std::shared_ptr<Subscriber::PublishNamespaceHandle>>
      publishNamespaceHandles_;
};

} // namespace moxygen::transports

#endif // MOXYGEN_QUIC_PICOQUIC
