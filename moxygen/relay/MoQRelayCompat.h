/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "moxygen/MoQSession.h"
#include "moxygen/relay/MoQForwarder.h"
#include "moxygen/compat/Containers.h"
#include "moxygen/compat/Hash.h"

#if !MOXYGEN_USE_FOLLY || !MOXYGEN_QUIC_MVFST

namespace moxygen {

/**
 * MoQRelayCompat - Simplified relay implementation for picoquic/std-mode
 *
 * This is a callback-based relay that handles the core relay functionality:
 * - PUBLISH_NAMESPACE from publishers
 * - SUBSCRIBE from subscribers
 * - Forward objects through MoQForwarder
 *
 * Unlike the full MoQRelay, this doesn't support:
 * - SUBSCRIBE_NAMESPACE
 * - Caching
 * - Multiple publishers for the same namespace
 */
class MoQRelayCompat : public Publisher,
                       public Subscriber,
                       public std::enable_shared_from_this<MoQRelayCompat>,
                       public MoQForwarder::Callback {
 public:
  explicit MoQRelayCompat() = default;

  void setAllowedNamespacePrefix(TrackNamespace allowed) {
    allowedNamespacePrefix_ = std::move(allowed);
  }

  // Publisher interface - callback-based
  void subscribeWithCallback(
      SubscribeRequest subReq,
      std::shared_ptr<TrackConsumer> consumer,
      std::shared_ptr<compat::ResultCallback<
          std::shared_ptr<Publisher::SubscriptionHandle>,
          SubscribeError>> callback) override;

  void fetchWithCallback(
      Fetch fetch,
      std::shared_ptr<FetchConsumer> consumer,
      std::shared_ptr<compat::ResultCallback<
          std::shared_ptr<Publisher::FetchHandle>,
          FetchError>> callback) override;

  void subscribeNamespaceWithCallback(
      SubscribeNamespace subAnn,
      std::shared_ptr<compat::ResultCallback<
          std::shared_ptr<Publisher::SubscribeNamespaceHandle>,
          SubscribeNamespaceError>> callback) override {
    // Not supported in compat mode
    callback->onError(SubscribeNamespaceError{
        subAnn.requestID,
        SubscribeNamespaceErrorCode::NOT_SUPPORTED,
        "SUBSCRIBE_NAMESPACE not supported in compat relay"});
  }

  // Subscriber interface - callback-based
  void publishNamespaceWithCallback(
      PublishNamespace ann,
      std::shared_ptr<Subscriber::PublishNamespaceCallback> cancelCallback,
      std::shared_ptr<compat::ResultCallback<
          std::shared_ptr<Subscriber::PublishNamespaceHandle>,
          PublishNamespaceError>> callback) override;

  PublishResult publish(
      PublishRequest pubReq,
      std::shared_ptr<Publisher::SubscriptionHandle> handle = nullptr) override;

  void goaway(Goaway goaway) override {
    XLOG(INFO) << "Processing goaway uri=" << goaway.newSessionUri;
  }

  std::shared_ptr<MoQSession> findPublishNamespaceSession(
      const TrackNamespace& ns);

 private:
  // MoQForwarder::Callback
  void onEmpty(MoQForwarder* forwarder) override;
  void forwardChanged(MoQForwarder* forwarder) override;

  // Namespace node for tracking publishers
  struct NamespaceNode : public Subscriber::PublishNamespaceHandle {
    explicit NamespaceNode(MoQRelayCompat& relay) : relay_(relay) {}

    void publishNamespaceDone() override;

    // Public wrapper for setting publish namespace ok (base class method is protected)
    void setOk(PublishNamespaceOk ok) {
      setPublishNamespaceOk(std::move(ok));
    }

    void requestUpdateWithCallback(
        RequestUpdate reqUpdate,
        std::shared_ptr<compat::ResultCallback<RequestOk, RequestError>>
            callback) override {
      callback->onError(RequestError{
          reqUpdate.requestID,
          RequestErrorCode::NOT_SUPPORTED,
          "REQUEST_UPDATE not supported for relay PUBLISH_NAMESPACE"});
    }

    TrackNamespace trackNamespace_;
    std::shared_ptr<MoQSession> sourceSession;
    std::shared_ptr<Subscriber::PublishNamespaceCallback>
        publishNamespaceCallback;
    MoQRelayCompat& relay_;
  };

  struct RelaySubscription {
    RelaySubscription(
        std::shared_ptr<MoQForwarder> f,
        std::shared_ptr<MoQSession> u)
        : forwarder(std::move(f)), upstream(std::move(u)) {}

    std::shared_ptr<MoQForwarder> forwarder;
    std::shared_ptr<MoQSession> upstream;
    RequestID requestID{0};
    std::shared_ptr<Publisher::SubscriptionHandle> handle;
    bool subscribed{false};
    bool isPublish{false};

    // Pending callbacks waiting for subscription to complete
    std::vector<std::pair<
        std::shared_ptr<MoQForwarder::Subscriber>,
        std::shared_ptr<compat::ResultCallback<
            std::shared_ptr<Publisher::SubscriptionHandle>,
            SubscribeError>>>>
        pendingCallbacks;
  };

  std::shared_ptr<NamespaceNode> findOrCreateNamespaceNode(
      const TrackNamespace& ns);
  std::shared_ptr<NamespaceNode> findNamespaceNode(const TrackNamespace& ns);

  TrackNamespace allowedNamespacePrefix_;

  // Map from namespace to the node tracking that namespace
  compat::FastMap<TrackNamespace, std::shared_ptr<NamespaceNode>,
                  TrackNamespace::hash> namespaceNodes_;

  // Map from full track name to relay subscription
  compat::FastMap<FullTrackName, RelaySubscription, FullTrackName::hash>
      subscriptions_;
};

} // namespace moxygen

#endif // !MOXYGEN_USE_FOLLY || !MOXYGEN_QUIC_MVFST
