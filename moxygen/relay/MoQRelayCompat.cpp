/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "moxygen/relay/MoQRelayCompat.h"
#include "moxygen/compat/Debug.h"

#include <iostream>

#if !MOXYGEN_USE_FOLLY || !MOXYGEN_QUIC_MVFST

namespace {
constexpr uint8_t kDefaultUpstreamPriority = 128;
}

namespace moxygen {

std::shared_ptr<MoQRelayCompat::NamespaceNode>
MoQRelayCompat::findOrCreateNamespaceNode(const TrackNamespace& ns) {
  auto it = namespaceNodes_.find(ns);
  if (it != namespaceNodes_.end()) {
    return it->second;
  }
  auto node = std::make_shared<NamespaceNode>(*this);
  node->trackNamespace_ = ns;
  namespaceNodes_.emplace(ns, node);
  return node;
}

std::shared_ptr<MoQRelayCompat::NamespaceNode>
MoQRelayCompat::findNamespaceNode(const TrackNamespace& ns) {
  // First try exact match
  auto it = namespaceNodes_.find(ns);
  if (it != namespaceNodes_.end()) {
    return it->second;
  }

  // Try prefix match - find the longest matching prefix
  std::shared_ptr<NamespaceNode> bestMatch;
  for (const auto& [nodeNs, node] : namespaceNodes_) {
    if (ns.startsWith(nodeNs)) {
      if (!bestMatch ||
          nodeNs.trackNamespace.size() >
              bestMatch->trackNamespace_.trackNamespace.size()) {
        bestMatch = node;
      }
    }
  }
  return bestMatch;
}

std::shared_ptr<MoQSession> MoQRelayCompat::findPublishNamespaceSession(
    const TrackNamespace& ns) {
  auto node = findNamespaceNode(ns);
  if (!node) {
    return nullptr;
  }
  return node->sourceSession;
}

void MoQRelayCompat::NamespaceNode::publishNamespaceDone() {
  XLOG(INFO) << "publishNamespaceDone ns=" << trackNamespace_;
  relay_.namespaceNodes_.erase(trackNamespace_);
}

void MoQRelayCompat::publishNamespaceWithCallback(
    PublishNamespace ann,
    std::shared_ptr<Subscriber::PublishNamespaceCallback> cancelCallback,
    std::shared_ptr<compat::ResultCallback<
        std::shared_ptr<Subscriber::PublishNamespaceHandle>,
        PublishNamespaceError>> callback) {
  XLOG(DBG1) << __func__ << " ns=" << ann.trackNamespace;
  std::cerr << "[RELAY] publishNamespaceWithCallback ns=" << ann.trackNamespace << "\n";

  // Check auth
  if (!ann.trackNamespace.startsWith(allowedNamespacePrefix_)) {
    callback->onError(PublishNamespaceError{
        ann.requestID,
        PublishNamespaceErrorCode::UNINTERESTED,
        "bad namespace"});
    return;
  }

  auto session = MoQSession::getRequestSession();
  auto nodePtr = findOrCreateNamespaceNode(ann.trackNamespace);

  // Check if there's already a publisher
  if (nodePtr->sourceSession) {
    XLOG(WARNING) << "PublishNamespace: Existing session already published "
                  << ann.trackNamespace;
    // Cancel old publisher
    if (nodePtr->publishNamespaceCallback) {
      nodePtr->publishNamespaceCallback->publishNamespaceCancel(
          PublishNamespaceErrorCode::CANCELLED, "New publisher");
      nodePtr->publishNamespaceCallback.reset();
    }
    nodePtr->sourceSession.reset();
  }

  nodePtr->sourceSession = std::move(session);
  nodePtr->publishNamespaceCallback = std::move(cancelCallback);
  nodePtr->setOk({ann.requestID});

  callback->onSuccess(nodePtr);
}

Subscriber::PublishResult MoQRelayCompat::publish(
    PublishRequest pub,
    std::shared_ptr<Publisher::SubscriptionHandle> handle) {
  XLOG(DBG1) << __func__ << " ftn=" << pub.fullTrackName;
  XCHECK(handle) << "Publish handle cannot be null";

  if (!pub.fullTrackName.trackNamespace.startsWith(allowedNamespacePrefix_)) {
    return compat::makeUnexpected(PublishError{
        pub.requestID, PublishErrorCode::UNINTERESTED, "bad namespace"});
  }

  if (pub.fullTrackName.trackNamespace.empty()) {
    return compat::makeUnexpected(PublishError{
        pub.requestID, PublishErrorCode::INTERNAL_ERROR, "namespace required"});
  }

  auto session = MoQSession::getRequestSession();

  auto it = subscriptions_.find(pub.fullTrackName);
  if (it != subscriptions_.end()) {
    // Someone already published this FTN
    XLOG(DBG1) << "New publisher for existing subscription";
    it->second.handle->unsubscribe();
    it->second.forwarder->publishDone(
        {RequestID(0),
         PublishDoneStatusCode::SUBSCRIPTION_ENDED,
         0,
         "upstream disconnect"});
    subscriptions_.erase(it);
  }

  // Create forwarder for this publish
  auto forwarder =
      std::make_shared<MoQForwarder>(pub.fullTrackName, std::nullopt);
  forwarder->setGroupOrder(pub.groupOrder);
  forwarder->setCallback(shared_from_this());

  auto subRes = subscriptions_.emplace(
      std::piecewise_construct,
      std::forward_as_tuple(pub.fullTrackName),
      std::forward_as_tuple(forwarder, session));
  auto& rsub = subRes.first->second;
  rsub.requestID = pub.requestID;
  rsub.handle = std::move(handle);
  rsub.isPublish = true;
  rsub.subscribed = true;

  // Return the forwarder as the consumer
  // In compat mode, publish returns immediately - the callback is invoked
  // by session when PublishOk is received
  return Subscriber::PublishConsumerAndReplyCallback{
      forwarder,
      nullptr  // No callback needed - we're the relay responding
  };
}

void MoQRelayCompat::subscribeWithCallback(
    SubscribeRequest subReq,
    std::shared_ptr<TrackConsumer> consumer,
    std::shared_ptr<compat::ResultCallback<
        std::shared_ptr<Publisher::SubscriptionHandle>,
        SubscribeError>> callback) {
  XLOG(DBG1) << __func__ << " ftn=" << subReq.fullTrackName;
  std::cerr << "[RELAY] subscribeWithCallback ftn=" << subReq.fullTrackName << "\n";

  auto session = MoQSession::getRequestSession();

  // Check if we already have a subscription for this track
  auto subscriptionIt = subscriptions_.find(subReq.fullTrackName);
  if (subscriptionIt != subscriptions_.end()) {
    // Existing subscription - add subscriber to forwarder
    auto& rsub = subscriptionIt->second;
    if (rsub.subscribed) {
      auto subscriber = rsub.forwarder->addSubscriber(
          std::move(session), subReq, std::move(consumer));
      if (!subscriber) {
        callback->onError(SubscribeError{
            subReq.requestID,
            SubscribeErrorCode::INTERNAL_ERROR,
            "failed to add subscriber"});
        return;
      }
      callback->onSuccess(subscriber);
    } else {
      // Subscription is pending - queue callback
      auto subscriber = rsub.forwarder->addSubscriber(
          std::move(session), subReq, std::move(consumer));
      if (!subscriber) {
        callback->onError(SubscribeError{
            subReq.requestID,
            SubscribeErrorCode::INTERNAL_ERROR,
            "failed to add subscriber"});
        return;
      }
      rsub.pendingCallbacks.emplace_back(subscriber, std::move(callback));
    }
    return;
  }

  // First subscriber - need to subscribe upstream
  if (subReq.fullTrackName.trackNamespace.empty()) {
    callback->onError(SubscribeError{
        subReq.requestID,
        SubscribeErrorCode::TRACK_NOT_EXIST,
        "namespace required"});
    return;
  }

  auto upstreamSession =
      findPublishNamespaceSession(subReq.fullTrackName.trackNamespace);
  std::cerr << "[RELAY] upstreamSession=" << upstreamSession.get()
            << " for ns=" << subReq.fullTrackName.trackNamespace << "\n";
  if (!upstreamSession) {
    std::cerr << "[RELAY] ERROR: No upstream session found!\n";
    callback->onError(SubscribeError{
        subReq.requestID,
        SubscribeErrorCode::TRACK_NOT_EXIST,
        "no such namespace or track"});
    return;
  }

  // Create forwarder
  auto forwarder =
      std::make_shared<MoQForwarder>(subReq.fullTrackName, std::nullopt);
  forwarder->setCallback(shared_from_this());

  auto emplaceRes = subscriptions_.emplace(
      std::piecewise_construct,
      std::forward_as_tuple(subReq.fullTrackName),
      std::forward_as_tuple(forwarder, upstreamSession));
  auto& rsub = emplaceRes.first->second;

  // Add subscriber first in case objects come before subscribe OK
  auto subscriber = forwarder->addSubscriber(
      std::move(session), subReq, std::move(consumer));
  if (!subscriber) {
    subscriptions_.erase(emplaceRes.first);
    callback->onError(SubscribeError{
        subReq.requestID,
        SubscribeErrorCode::INTERNAL_ERROR,
        "failed to add subscriber"});
    return;
  }

  rsub.pendingCallbacks.emplace_back(subscriber, std::move(callback));

  // Prepare upstream subscribe request
  subReq.priority = kDefaultUpstreamPriority;
  subReq.groupOrder = GroupOrder::Default;
  subReq.locType = LocationType::LargestObject;
  subReq.forward = forwarder->numForwardingSubscribers() > 0;

  auto ftn = subReq.fullTrackName;

  // Subscribe upstream with callback
  std::cerr << "[RELAY] Subscribing upstream to session=" << upstreamSession.get() << "\n";
  upstreamSession->subscribeWithCallback(
      subReq,
      forwarder,
      compat::makeResultCallback<
          std::shared_ptr<Publisher::SubscriptionHandle>,
          SubscribeError>(
          [self = shared_from_this(), ftn](
              std::shared_ptr<Publisher::SubscriptionHandle> handle) {
            std::cerr << "[RELAY] Upstream subscribe OK for " << ftn << "\n";
            auto it = self->subscriptions_.find(ftn);
            if (it == self->subscriptions_.end()) {
              handle->unsubscribe();
              return;
            }
            auto& rsub = it->second;
            rsub.handle = std::move(handle);
            rsub.subscribed = true;

            // Get subscribe info from handle
            auto& subOk = rsub.handle->subscribeOk();
            if (subOk.largest) {
              rsub.forwarder->updateLargest(
                  subOk.largest->group, subOk.largest->object);
            }
            rsub.forwarder->setGroupOrder(subOk.groupOrder);

            // Notify all pending callbacks
            for (auto& [sub, cb] : rsub.pendingCallbacks) {
              if (subOk.largest) {
                sub->updateLargest(*subOk.largest);
              }
              sub->setPublisherGroupOrder(subOk.groupOrder);
              cb->onSuccess(sub);
            }
            rsub.pendingCallbacks.clear();
          },
          [self = shared_from_this(), ftn](const SubscribeError& error) {
            std::cerr << "[RELAY] Upstream subscribe FAILED for " << ftn
                      << " error=" << error.reasonPhrase << "\n";
            auto it = self->subscriptions_.find(ftn);
            if (it == self->subscriptions_.end()) {
              return;
            }
            auto& rsub = it->second;

            // Notify all pending callbacks of failure
            for (auto& [sub, cb] : rsub.pendingCallbacks) {
              cb->onError(error);
            }
            rsub.pendingCallbacks.clear();
            self->subscriptions_.erase(it);
          }));
}

void MoQRelayCompat::fetchWithCallback(
    Fetch fetch,
    std::shared_ptr<FetchConsumer> consumer,
    std::shared_ptr<compat::ResultCallback<
        std::shared_ptr<Publisher::FetchHandle>,
        FetchError>> callback) {
  XLOG(DBG1) << __func__ << " ftn=" << fetch.fullTrackName;

  if (fetch.fullTrackName.trackNamespace.empty()) {
    callback->onError(FetchError{
        fetch.requestID, FetchErrorCode::TRACK_NOT_EXIST, "namespace required"});
    return;
  }

  auto upstreamSession =
      findPublishNamespaceSession(fetch.fullTrackName.trackNamespace);
  if (!upstreamSession) {
    // Try to find from subscriptions
    auto subscriptionIt = subscriptions_.find(fetch.fullTrackName);
    if (subscriptionIt != subscriptions_.end()) {
      upstreamSession = subscriptionIt->second.upstream;
    }
    if (!upstreamSession) {
      callback->onError(FetchError{
          fetch.requestID,
          FetchErrorCode::TRACK_NOT_EXIST,
          "no such namespace"});
      return;
    }
  }

  fetch.priority = kDefaultUpstreamPriority;
  upstreamSession->fetchWithCallback(
      fetch, std::move(consumer), std::move(callback));
}

void MoQRelayCompat::onEmpty(MoQForwarder* forwarder) {
  auto subscriptionIt = subscriptions_.find(forwarder->fullTrackName());
  if (subscriptionIt == subscriptions_.end()) {
    return;
  }
  auto& subscription = subscriptionIt->second;

  if (!subscription.handle) {
    // Handle is null - publisher terminated
    XLOG(INFO) << "Publisher terminated for " << subscriptionIt->first;
    subscriptions_.erase(subscriptionIt);
    return;
  }

  // Handle exists - just last subscriber left
  XLOG(INFO) << "Last subscriber removed for " << subscriptionIt->first;
  if (!subscription.isPublish) {
    subscription.handle->unsubscribe();
    subscriptions_.erase(subscriptionIt);
  }
}

void MoQRelayCompat::forwardChanged(MoQForwarder* forwarder) {
  auto subscriptionIt = subscriptions_.find(forwarder->fullTrackName());
  if (subscriptionIt == subscriptions_.end()) {
    return;
  }
  auto& subscription = subscriptionIt->second;
  if (!subscription.subscribed) {
    // Ignore: still pending
    return;
  }
  XLOG(INFO) << "Forward changed for " << subscriptionIt->first
             << " numForwardingSubs=" << forwarder->numForwardingSubscribers();

  // TODO: Send REQUEST_UPDATE to update forward flag
}

} // namespace moxygen

#endif // !MOXYGEN_USE_FOLLY || !MOXYGEN_QUIC_MVFST
