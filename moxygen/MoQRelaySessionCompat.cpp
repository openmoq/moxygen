/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

// Callback-based (compat) implementation of MoQRelaySession
// This file is compiled when NOT using mvfst (std-mode or Folly+picoquic)

#include <moxygen/MoQRelaySession.h>

#if !MOXYGEN_QUIC_MVFST

namespace moxygen {

// Inner class implementations

class MoQRelaySession::PublisherPublishNamespaceHandle
    : public Subscriber::PublishNamespaceHandle {
 public:
  PublisherPublishNamespaceHandle(
      std::shared_ptr<MoQRelaySession> session,
      TrackNamespace trackNamespace,
      PublishNamespaceOk annOk)
      : Subscriber::PublishNamespaceHandle(std::move(annOk)),
        trackNamespace_(std::move(trackNamespace)),
        session_(std::move(session)) {}

  ~PublisherPublishNamespaceHandle() override {
    publishNamespaceDone();
  }

  void publishNamespaceDone() override {
    if (session_) {
      PublishNamespaceDone unann;
      if (session_->getNegotiatedVersion() &&
          getDraftMajorVersion(*session_->getNegotiatedVersion()) >= 16) {
        unann.requestID = publishNamespaceOk().requestID;
      } else {
        unann.trackNamespace = trackNamespace_;
      }
      session_->publishNamespaceDone(unann);
      session_.reset();
    }
  }

 private:
  TrackNamespace trackNamespace_;
  std::shared_ptr<MoQRelaySession> session_;
};

class MoQRelaySession::SubscribeNamespaceHandle
    : public Publisher::SubscribeNamespaceHandle {
 public:
  SubscribeNamespaceHandle(
      std::shared_ptr<MoQRelaySession> session,
      TrackNamespace trackNamespacePrefix,
      SubscribeNamespaceOk subAnnOk)
      : Publisher::SubscribeNamespaceHandle(std::move(subAnnOk)),
        trackNamespacePrefix_(std::move(trackNamespacePrefix)),
        session_(std::move(session)) {}

  ~SubscribeNamespaceHandle() override {
    unsubscribeNamespace();
  }

  void unsubscribeNamespace() override {
    if (session_) {
      UnsubscribeNamespace msg;
      if (session_->getNegotiatedVersion() &&
          getDraftMajorVersion(*session_->getNegotiatedVersion()) >= 15) {
        msg.requestID = subscribeNamespaceOk_->requestID;
      } else {
        msg.trackNamespacePrefix = trackNamespacePrefix_;
      }
      session_->unsubscribeNamespace(msg);
      session_.reset();
    }
  }

 private:
  TrackNamespace trackNamespacePrefix_;
  std::shared_ptr<MoQRelaySession> session_;
};

// Factory
std::function<std::shared_ptr<MoQSession>(
    std::shared_ptr<compat::WebTransportInterface>,
    std::shared_ptr<MoQExecutor>)>
MoQRelaySession::createRelaySessionFactory() {
  static auto factory =
      [](std::shared_ptr<compat::WebTransportInterface> wt,
         std::shared_ptr<MoQExecutor> exec) -> std::shared_ptr<MoQSession> {
    return std::static_pointer_cast<MoQSession>(
        std::make_shared<MoQRelaySession>(std::move(wt), std::move(exec)));
  };
  return factory;
}

void MoQRelaySession::cleanup() {
  // Clean up publishNamespace maps
  for (auto& ann : publishNamespaceCallbacks_) {
    if (ann.second) {
      ann.second->publishNamespaceCancel(
          PublishNamespaceErrorCode::INTERNAL_ERROR, "Session ended");
    }
  }
  publishNamespaceCallbacks_.clear();
  legacyPublisherNamespaceToReqId_.clear();

  for (auto& ann : publishNamespaceHandles_) {
    ann.second->publishNamespaceDone();
  }
  publishNamespaceHandles_.clear();
  legacySubscriberNamespaceToReqId_.clear();

  // Clean up subscribeNamespace handles
  for (auto& subAnn : subscribeNamespaceHandles_) {
    if (subAnn.second) {
      subAnn.second->unsubscribeNamespace();
    }
  }
  subscribeNamespaceHandles_.clear();
  legacySubscribeNamespaceToReqId_.clear();

  // Fail pending requests
  for (auto& pending : pendingPublishNamespaces_) {
    if (pending.second.callback) {
      pending.second.callback->onError(PublishNamespaceError{
          pending.first,
          PublishNamespaceErrorCode::INTERNAL_ERROR,
          "Session ended"});
    }
  }
  pendingPublishNamespaces_.clear();

  for (auto& pending : pendingSubscribeNamespaces_) {
    if (pending.second.callback) {
      pending.second.callback->onError(SubscribeNamespaceError{
          pending.first,
          SubscribeNamespaceErrorCode::INTERNAL_ERROR,
          "Session ended"});
    }
  }
  pendingSubscribeNamespaces_.clear();

  // Call parent cleanup
  MoQSession::cleanup();
}

// Publisher methods (outgoing publishNamespace)
void MoQRelaySession::publishNamespaceWithCallback(
    PublishNamespace ann,
    std::shared_ptr<Subscriber::PublishNamespaceCallback> cancelCallback,
    std::shared_ptr<compat::ResultCallback<
        std::shared_ptr<Subscriber::PublishNamespaceHandle>,
        PublishNamespaceError>> callback) {
  XLOG(INFO) << __func__ << " ns=" << ann.trackNamespace << " sess=" << this;

  const auto& trackNamespace = ann.trackNamespace;
  ann.requestID = getNextRequestID();

  auto res = moqFrameWriter_.writePublishNamespace(controlWriteBuf_, ann);
  if (!res) {
    LOG(ERROR) << "writePublishNamespace failed sess=" << this;
    callback->onError(PublishNamespaceError{
        ann.requestID,
        PublishNamespaceErrorCode::INTERNAL_ERROR,
        "local write failed"});
    return;
  }

  pendingPublishNamespaces_.emplace(
      ann.requestID,
      PendingPublishNamespace{
          trackNamespace, std::move(callback), std::move(cancelCallback)});

  flushControlBuf();
}

// Subscriber methods (outgoing subscribeNamespace)
void MoQRelaySession::subscribeNamespaceWithCallback(
    SubscribeNamespace subAnn,
    std::shared_ptr<compat::ResultCallback<
        std::shared_ptr<Publisher::SubscribeNamespaceHandle>,
        SubscribeNamespaceError>> callback) {
  XLOG(INFO) << __func__ << " prefix=" << subAnn.trackNamespacePrefix
            << " sess=" << this;

  const auto& trackNamespace = subAnn.trackNamespacePrefix;
  subAnn.requestID = getNextRequestID();

  auto res = moqFrameWriter_.writeSubscribeNamespace(controlWriteBuf_, subAnn);
  if (!res) {
    LOG(ERROR) << "writeSubscribeNamespace failed sess=" << this;
    callback->onError(SubscribeNamespaceError{
        subAnn.requestID,
        SubscribeNamespaceErrorCode::INTERNAL_ERROR,
        "local write failed"});
    return;
  }

  pendingSubscribeNamespaces_.emplace(
      subAnn.requestID,
      PendingSubscribeNamespace{trackNamespace, std::move(callback)});

  flushControlBuf();
}

// Incoming message handlers
void MoQRelaySession::onPublishNamespace(PublishNamespace ann) {
  XLOG(INFO) << __func__ << " ns=" << ann.trackNamespace << " sess=" << this;

  if (!subscribeHandler_) {
    XLOG(INFO) << __func__ << " No subscriber callback set";
    publishNamespaceError(PublishNamespaceError{
        ann.requestID,
        PublishNamespaceErrorCode::NOT_SUPPORTED,
        "Not a subscriber"});
    return;
  }

  // TODO: Implement proper callback handling for incoming publishNamespace
  // For now, return NOT_SUPPORTED
  publishNamespaceError(PublishNamespaceError{
      ann.requestID,
      PublishNamespaceErrorCode::NOT_SUPPORTED,
      "Callback handling not fully implemented"});
}

void MoQRelaySession::onPublishNamespaceCancel(
    PublishNamespaceCancel publishNamespaceCancel) {
  XLOG(INFO) << __func__ << " sess=" << this;

  RequestID reqId;
  if (getNegotiatedVersion() &&
      getDraftMajorVersion(*getNegotiatedVersion()) >= 16) {
    if (!publishNamespaceCancel.requestID.has_value()) {
      return;
    }
    reqId = *publishNamespaceCancel.requestID;
  } else {
    auto nsIt = legacyPublisherNamespaceToReqId_.find(
        publishNamespaceCancel.trackNamespace);
    if (nsIt == legacyPublisherNamespaceToReqId_.end()) {
      LOG(ERROR) << "Invalid publishNamespace cancel";
      return;
    }
    reqId = nsIt->second;
    legacyPublisherNamespaceToReqId_.erase(nsIt);
  }

  auto it = publishNamespaceCallbacks_.find(reqId);
  if (it != publishNamespaceCallbacks_.end()) {
    auto cb = std::move(it->second);
    publishNamespaceCallbacks_.erase(it);
    if (cb) {
      cb->publishNamespaceCancel(
          publishNamespaceCancel.errorCode,
          std::move(publishNamespaceCancel.reasonPhrase));
    }
  }
}

void MoQRelaySession::onPublishNamespaceDone(PublishNamespaceDone unAnn) {
  XLOG(INFO) << __func__ << " sess=" << this;

  RequestID reqId;
  if (getNegotiatedVersion() &&
      getDraftMajorVersion(*getNegotiatedVersion()) >= 16) {
    if (!unAnn.requestID.has_value()) {
      return;
    }
    reqId = *unAnn.requestID;
  } else {
    auto nsIt = legacySubscriberNamespaceToReqId_.find(unAnn.trackNamespace);
    if (nsIt == legacySubscriberNamespaceToReqId_.end()) {
      LOG(ERROR) << "PublishNamespaceDone for unknown namespace";
      return;
    }
    reqId = nsIt->second;
    legacySubscriberNamespaceToReqId_.erase(nsIt);
  }

  auto it = publishNamespaceHandles_.find(reqId);
  if (it != publishNamespaceHandles_.end()) {
    auto handle = std::move(it->second);
    publishNamespaceHandles_.erase(it);
    handle->publishNamespaceDone();
  }

  retireRequestID(/*signalWriteLoop=*/true);
}

void MoQRelaySession::onSubscribeNamespace(SubscribeNamespace sa) {
  XLOG(INFO) << __func__ << " prefix=" << sa.trackNamespacePrefix
            << " sess=" << this;

  if (!publishHandler_) {
    XLOG(INFO) << __func__ << " No publisher callback set";
    subscribeNamespaceError(SubscribeNamespaceError{
        sa.requestID,
        SubscribeNamespaceErrorCode::NOT_SUPPORTED,
        "Not a publisher"});
    return;
  }

  // TODO: Implement callback handling
  subscribeNamespaceError(SubscribeNamespaceError{
      sa.requestID,
      SubscribeNamespaceErrorCode::NOT_SUPPORTED,
      "Callback handling not fully implemented"});
}

void MoQRelaySession::onRequestOk(RequestOk requestOk, FrameType frameType) {
  XLOG(INFO) << __func__ << " id=" << requestOk.requestID << " sess=" << this;

  // Check pending publishNamespace
  auto pubIt = pendingPublishNamespaces_.find(requestOk.requestID);
  if (pubIt != pendingPublishNamespaces_.end()) {
    auto pending = std::move(pubIt->second);
    pendingPublishNamespaces_.erase(pubIt);

    // Store the cancel callback
    publishNamespaceCallbacks_[requestOk.requestID] =
        std::move(pending.cancelCallback);
    if (getNegotiatedVersion() &&
        getDraftMajorVersion(*getNegotiatedVersion()) < 16) {
      legacyPublisherNamespaceToReqId_[pending.trackNamespace] =
          requestOk.requestID;
    }

    // Create handle and call success
    auto handle = std::make_shared<PublisherPublishNamespaceHandle>(
        std::static_pointer_cast<MoQRelaySession>(shared_from_this()),
        pending.trackNamespace,
        requestOk);
    pending.callback->onSuccess(std::move(handle));
    return;
  }

  // Check pending subscribeNamespace
  auto subIt = pendingSubscribeNamespaces_.find(requestOk.requestID);
  if (subIt != pendingSubscribeNamespaces_.end()) {
    auto pending = std::move(subIt->second);
    pendingSubscribeNamespaces_.erase(subIt);

    // Create handle and call success
    auto handle = std::make_shared<SubscribeNamespaceHandle>(
        std::static_pointer_cast<MoQRelaySession>(shared_from_this()),
        pending.trackNamespacePrefix,
        requestOk);
    subscribeNamespaceHandles_[requestOk.requestID] = handle;
    if (getNegotiatedVersion() &&
        getDraftMajorVersion(*getNegotiatedVersion()) < 15) {
      legacySubscribeNamespaceToReqId_[pending.trackNamespacePrefix] =
          requestOk.requestID;
    }
    pending.callback->onSuccess(std::move(handle));
    return;
  }

  // Fall through to base class
  MoQSession::onRequestOk(requestOk, frameType);
}

void MoQRelaySession::onUnsubscribeNamespace(UnsubscribeNamespace unsub) {
  XLOG(INFO) << __func__ << " sess=" << this;

  RequestID requestID;
  if (getNegotiatedVersion() &&
      getDraftMajorVersion(*getNegotiatedVersion()) >= 15) {
    if (!unsub.requestID.has_value()) {
      LOG(ERROR) << "Missing requestID for v15+";
      return;
    }
    requestID = *unsub.requestID;
  } else {
    if (!unsub.trackNamespacePrefix.has_value()) {
      LOG(ERROR) << "Missing trackNamespacePrefix for <v15";
      return;
    }
    auto nsIt =
        legacySubscribeNamespaceToReqId_.find(*unsub.trackNamespacePrefix);
    if (nsIt == legacySubscribeNamespaceToReqId_.end()) {
      LOG(ERROR) << "Invalid unsubscribe namespace";
      return;
    }
    requestID = nsIt->second;
    legacySubscribeNamespaceToReqId_.erase(nsIt);
  }

  auto it = subscribeNamespaceHandles_.find(requestID);
  if (it != subscribeNamespaceHandles_.end()) {
    auto handle = std::move(it->second);
    subscribeNamespaceHandles_.erase(it);
    handle->unsubscribeNamespace();
  }

  retireRequestID(/*signalWriteLoop=*/true);
}

// Internal methods
void MoQRelaySession::subscribeNamespaceOk(const SubscribeNamespaceOk& saOk) {
  XLOG(INFO) << __func__ << " id=" << saOk.requestID << " sess=" << this;
  auto res = moqFrameWriter_.writeSubscribeNamespaceOk(controlWriteBuf_, saOk);
  if (!res) {
    LOG(ERROR) << "writeSubscribeNamespaceOk failed";
  }
}

void MoQRelaySession::unsubscribeNamespace(const UnsubscribeNamespace& unsub) {
  XLOG(INFO) << __func__ << " sess=" << this;
  auto res =
      moqFrameWriter_.writeUnsubscribeNamespace(controlWriteBuf_, unsub);
  if (!res) {
    LOG(ERROR) << "writeUnsubscribeNamespace failed";
  }
}

void MoQRelaySession::publishNamespaceOk(const PublishNamespaceOk& annOk) {
  XLOG(INFO) << __func__ << " reqID=" << annOk.requestID << " sess=" << this;
  auto res = moqFrameWriter_.writePublishNamespaceOk(controlWriteBuf_, annOk);
  if (!res) {
    LOG(ERROR) << "writePublishNamespaceOk failed";
  }
}

void MoQRelaySession::publishNamespaceCancel(
    const PublishNamespaceCancel& annCan) {
  auto res =
      moqFrameWriter_.writePublishNamespaceCancel(controlWriteBuf_, annCan);
  if (!res) {
    LOG(ERROR) << "writePublishNamespaceCancel failed";
  }
  if (annCan.requestID.has_value()) {
    publishNamespaceHandles_.erase(*annCan.requestID);
  }
  retireRequestID(/*signalWriteLoop=*/false);
}

void MoQRelaySession::publishNamespaceDone(
    const PublishNamespaceDone& unann) {
  auto res =
      moqFrameWriter_.writePublishNamespaceDone(controlWriteBuf_, unann);
  if (!res) {
    LOG(ERROR) << "writePublishNamespaceDone failed";
  }
}

} // namespace moxygen

#endif // !MOXYGEN_QUIC_MVFST
