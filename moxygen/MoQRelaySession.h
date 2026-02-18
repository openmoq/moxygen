/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <moxygen/MoQSession.h>
#include <moxygen/compat/Containers.h>

namespace moxygen {

/**
 * MoQRelaySession extends MoQSession with full publishNamespace
 * functionality.
 *
 * This subclass provides real implementations of publishNamespace() and
 * subscribeNamespace() methods, along with proper publishNamespace
 * state management. It should be used in relay servers and any applications
 * that need to handle publishNamespaces.
 *
 * The base MoQSession returns NOT_SUPPORTED for publishNamespace
 * operations, making it suitable for simple clients that only subscribe to
 * tracks.
 */

#if MOXYGEN_USE_FOLLY && MOXYGEN_QUIC_MVFST

class MoQRelaySession : public MoQSession {
 public:
  // Inherit all base constructors
  using MoQSession::MoQSession;

  // Static factory for creating relay sessions in clients
  static std::function<std::shared_ptr<MoQSession>(
      folly::MaybeManagedPtr<proxygen::WebTransport>,
      MoQExecutor::KeepAlive)>
  createRelaySessionFactory();

  // Override cleanup method for proper inheritance pattern
  void cleanup() override;

  // Override publishNamespace methods with real implementations
  folly::coro::Task<Subscriber::PublishNamespaceResult> publishNamespace(
      PublishNamespace ann,
      std::shared_ptr<PublishNamespaceCallback> publishNamespaceCallback =
          nullptr) override;

  folly::coro::Task<Publisher::SubscribeNamespaceResult> subscribeNamespace(
      SubscribeNamespace subAnn,
      std::shared_ptr<NamespacePublishHandle> namespacePublishHandle) override;

 private:
  // Forward declarations for inner classes
  class SubscriberPublishNamespaceCallback;
  class PublisherPublishNamespaceHandle;
  class SubscribeNamespaceHandle;

  // Override to handle ANNOUNCE and SUBSCRIBE_ANNOUNCES updates
  void onRequestUpdate(RequestUpdate requestUpdate) override;

  // REQUEST_UPDATE handlers for announcement types - take handles directly
  void handlePublishNamespaceRequestUpdate(
      RequestUpdate requestUpdate,
      std::shared_ptr<Subscriber::PublishNamespaceHandle> announceHandle);
  void handleSubscribeNamespaceRequestUpdate(
      RequestUpdate requestUpdate,
      std::shared_ptr<Publisher::SubscribeNamespaceHandle>
          subscribeNamespaceHandle);

  // Internal publishNamespace handling methods
  folly::coro::Task<void> handleSubscribeNamespace(SubscribeNamespace sa);
  void subscribeNamespaceOk(const SubscribeNamespaceOk& saOk);
  void unsubscribeNamespace(const UnsubscribeNamespace& unsubAnn);

  folly::coro::Task<void> handlePublishNamespace(
      PublishNamespace publishNamespace);
  void publishNamespaceOk(const PublishNamespaceOk& annOk);
  void publishNamespaceCancel(const PublishNamespaceCancel& annCan);
  void publishNamespaceDone(const PublishNamespaceDone& publishNamespaceDone);

  // Override all incoming publishNamespace message handlers
  void onPublishNamespace(PublishNamespace ann) override;
  void onPublishNamespaceCancel(
      PublishNamespaceCancel publishNamespaceCancel) override;
  void onPublishNamespaceDone(PublishNamespaceDone unAnn) override;
  void onSubscribeNamespace(SubscribeNamespace sa) override;
  void onRequestOk(RequestOk ok, FrameType frameType) override;
  void onUnsubscribeNamespace(UnsubscribeNamespace unsub) override;

  // Helper methods for handling RequestOk for different request types
  void handlePublishNamespaceOkFromRequestOk(
      const RequestOk& requestOk,
      PendingRequestIterator reqIt);
  void handleSubscribeNamespaceOkFromRequestOk(
      const RequestOk& requestOk,
      PendingRequestIterator reqIt);

  // PublishNamespace-specific types (moved from base class)
  struct PendingPublishNamespace {
    TrackNamespace trackNamespace;
    folly::coro::Promise<
        folly::Expected<PublishNamespaceOk, PublishNamespaceError>>
        promise;
    std::shared_ptr<PublishNamespaceCallback> callback;
  };

  // PublishNamespace state management
  // Primary maps keyed by RequestID
  folly::F14FastMap<
      RequestID,
      std::shared_ptr<Subscriber::PublishNamespaceHandle>,
      RequestID::hash>
      publishNamespaceHandles_;
  folly::F14FastMap<
      RequestID,
      std::shared_ptr<Subscriber::PublishNamespaceCallback>,
      RequestID::hash>
      publishNamespaceCallbacks_;
  folly::F14FastMap<
      RequestID,
      std::shared_ptr<Publisher::SubscribeNamespaceHandle>,
      RequestID::hash>
      subscribeNamespaceHandles_;

  // Legacy TrackNamespace → RequestID translation maps.
  // Remove these once we drop support for the respective legacy versions.
  // legacyPublisherPublishNamespaceNsToReqId_: v15- (publisher side)
  // legacySubscriberPublishNamespaceNsToReqId_: v15- (subscriber side)
  // legacySubscribeNamespaceNsToReqId_: v14- (subscribe namespace)
  folly::F14FastMap<TrackNamespace, RequestID, TrackNamespace::hash>
      legacyPublisherNamespaceToReqId_;
  folly::F14FastMap<TrackNamespace, RequestID, TrackNamespace::hash>
      legacySubscriberNamespaceToReqId_;
  folly::F14FastMap<TrackNamespace, RequestID, TrackNamespace::hash>
      legacySubscribeNamespaceToReqId_;

  // Extended PendingRequestState for publishNamespace support
  class MoQRelayPendingRequestState;
};

#else // !MOXYGEN_USE_FOLLY || !MOXYGEN_QUIC_MVFST

/**
 * MoQRelaySession - Std-mode implementation with callback-based API
 *
 * Provides namespace functionality using callbacks instead of coroutines.
 */
class MoQRelaySession : public MoQSession {
 public:
  // Inherit all base constructors
  using MoQSession::MoQSession;

  // Static factory for creating relay sessions
  static std::function<std::shared_ptr<MoQSession>(
      std::shared_ptr<compat::WebTransportInterface>,
      std::shared_ptr<MoQExecutor>)>
  createRelaySessionFactory();

  // Override cleanup method
  void cleanup() override;

  // Callback-based publishNamespace
  void publishNamespaceWithCallback(
      PublishNamespace ann,
      std::shared_ptr<Subscriber::PublishNamespaceCallback> cancelCallback,
      std::shared_ptr<compat::ResultCallback<
          std::shared_ptr<Subscriber::PublishNamespaceHandle>,
          PublishNamespaceError>> callback) override;

  // Callback-based subscribeNamespace
  void subscribeNamespaceWithCallback(
      SubscribeNamespace subAnn,
      std::shared_ptr<compat::ResultCallback<
          std::shared_ptr<Publisher::SubscribeNamespaceHandle>,
          SubscribeNamespaceError>> callback) override;

 private:
  // Forward declarations for inner classes
  class PublisherPublishNamespaceHandle;
  class SubscribeNamespaceHandle;

  // Internal methods
  void subscribeNamespaceOk(const SubscribeNamespaceOk& saOk);
  void unsubscribeNamespace(const UnsubscribeNamespace& unsubAnn);
  void publishNamespaceOk(const PublishNamespaceOk& annOk);
  void publishNamespaceCancel(const PublishNamespaceCancel& annCan);
  void publishNamespaceDone(const PublishNamespaceDone& publishNamespaceDone);

  // Override incoming message handlers
  void onPublishNamespace(PublishNamespace ann) override;
  void onPublishNamespaceCancel(
      PublishNamespaceCancel publishNamespaceCancel) override;
  void onPublishNamespaceDone(PublishNamespaceDone unAnn) override;
  void onSubscribeNamespace(SubscribeNamespace sa) override;
  void onRequestOk(RequestOk ok, FrameType frameType) override;
  void onUnsubscribeNamespace(UnsubscribeNamespace unsub) override;

  // Pending namespace callbacks
  struct PendingPublishNamespace {
    TrackNamespace trackNamespace;
    std::shared_ptr<compat::ResultCallback<
        std::shared_ptr<Subscriber::PublishNamespaceHandle>,
        PublishNamespaceError>> callback;
    std::shared_ptr<Subscriber::PublishNamespaceCallback> cancelCallback;
  };

  struct PendingSubscribeNamespace {
    TrackNamespace trackNamespacePrefix;
    std::shared_ptr<compat::ResultCallback<
        std::shared_ptr<Publisher::SubscribeNamespaceHandle>,
        SubscribeNamespaceError>> callback;
  };

  // State management using compat containers
  compat::FastMap<
      RequestID,
      std::shared_ptr<Subscriber::PublishNamespaceHandle>,
      RequestID::hash>
      publishNamespaceHandles_;
  compat::FastMap<
      RequestID,
      std::shared_ptr<Subscriber::PublishNamespaceCallback>,
      RequestID::hash>
      publishNamespaceCallbacks_;
  compat::FastMap<
      RequestID,
      std::shared_ptr<Publisher::SubscribeNamespaceHandle>,
      RequestID::hash>
      subscribeNamespaceHandles_;

  // Pending requests
  compat::FastMap<RequestID, PendingPublishNamespace, RequestID::hash>
      pendingPublishNamespaces_;
  compat::FastMap<RequestID, PendingSubscribeNamespace, RequestID::hash>
      pendingSubscribeNamespaces_;

  // Legacy namespace to RequestID maps
  compat::FastMap<TrackNamespace, RequestID, TrackNamespace::hash>
      legacyPublisherNamespaceToReqId_;
  compat::FastMap<TrackNamespace, RequestID, TrackNamespace::hash>
      legacySubscriberNamespaceToReqId_;
  compat::FastMap<TrackNamespace, RequestID, TrackNamespace::hash>
      legacySubscribeNamespaceToReqId_;
};

#endif // MOXYGEN_USE_FOLLY && MOXYGEN_QUIC_MVFST

} // namespace moxygen
