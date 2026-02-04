/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// Std-mode (non-Folly) implementation of MoQSession
// This file is only compiled when MOXYGEN_USE_FOLLY=OFF

#include "moxygen/MoQSession.h"

#if !MOXYGEN_USE_FOLLY

namespace moxygen {

// Forward declarations of nested classes
class MoQSession::TrackReceiveStateBase {
 public:
  virtual ~TrackReceiveStateBase() = default;
};

class MoQSession::SubscribeTrackReceiveState : public TrackReceiveStateBase {
 public:
  SubscribeTrackReceiveState() = default;
};

class MoQSession::FetchTrackReceiveState : public TrackReceiveStateBase {
 public:
  FetchTrackReceiveState() = default;
};

class MoQSession::TrackPublisherImpl : public MoQSession::PublisherImpl {
 public:
  using PublisherImpl::PublisherImpl;
  void terminatePublish(SubscribeDone, ResetStreamErrorCode) override {}
  void onStreamComplete(const ObjectHeader&) override {}
  void onTooManyBytesBuffered() override {}
};

class MoQSession::FetchPublisherImpl : public MoQSession::PublisherImpl {
 public:
  using PublisherImpl::PublisherImpl;
  void terminatePublish(SubscribeDone, ResetStreamErrorCode) override {}
  void onStreamComplete(const ObjectHeader&) override {}
  void onTooManyBytesBuffered() override {}
};

// Constructor implementations
MoQSession::MoQSession(
    std::shared_ptr<compat::WebTransportInterface> wt,
    std::shared_ptr<MoQExecutor> exec)
    : dir_(MoQControlCodec::Direction::CLIENT),
      wt_(std::move(wt)),
      exec_(std::move(exec)),
      controlCodec_(dir_, this) {}

MoQSession::MoQSession(
    std::shared_ptr<compat::WebTransportInterface> wt,
    ServerSetupCallback& serverSetupCallback,
    std::shared_ptr<MoQExecutor> exec)
    : dir_(MoQControlCodec::Direction::SERVER),
      wt_(std::move(wt)),
      exec_(std::move(exec)),
      serverSetupCallback_(&serverSetupCallback),
      controlCodec_(dir_, this) {}

MoQSession::~MoQSession() {
  cleanup();
}

void MoQSession::cleanup() {
  cancellationSource_.requestCancellation();
  subscribeHandler_.reset();
  publishHandler_.reset();
  subTracks_.clear();
  fetches_.clear();
  pubTracks_.clear();
}

void MoQSession::setServerMaxTokenCacheSizeGuess(size_t size) {
  tokenCache_.setMaxSize(size);
}

void MoQSession::setVersion(uint64_t version) {
  moqFrameWriter_.initializeVersion(version);
}

void MoQSession::setMoqSettings(MoQSettings settings) {
  moqSettings_ = std::move(settings);
}

void MoQSession::setPublishHandler(std::shared_ptr<Publisher> publishHandler) {
  publishHandler_ = std::move(publishHandler);
}

void MoQSession::setSubscribeHandler(
    std::shared_ptr<Subscriber> subscribeHandler) {
  subscribeHandler_ = std::move(subscribeHandler);
}

void MoQSession::start() {
  if (!wt_) {
    LOG(ERROR) << "Cannot start session without transport";
    return;
  }
  // TODO: Set up control stream and callbacks
}

void MoQSession::drain() {
  draining_ = true;
}

void MoQSession::close(SessionCloseErrorCode error) {
  if (closed_) {
    return;
  }
  closed_ = true;
  cleanup();

  if (wt_) {
    wt_->closeSession(static_cast<uint32_t>(error));
  }

  if (closeCallback_) {
    closeCallback_->onMoQSessionClosed();
  }
}

void MoQSession::goaway(Goaway goway) {
  auto res = moqFrameWriter_.writeGoaway(controlWriteBuf_, goway);
  if (res.hasError()) {
    LOG(ERROR) << "Failed to write GOAWAY";
  }
}

void MoQSession::setupWithCallback(
    ClientSetup setup,
    std::shared_ptr<compat::ResultCallback<ServerSetup, SessionCloseErrorCode>>
        callback) {
  setupCallback_ = std::move(callback);

  // Use standalone function, not method
  auto res = writeClientSetup(
      controlWriteBuf_, setup, setup.supportedVersions[0]);
  if (res.hasError()) {
    if (setupCallback_) {
      setupCallback_->onError(SessionCloseErrorCode::INTERNAL_ERROR);
      setupCallback_.reset();
    }
  }
}

void MoQSession::setMaxConcurrentRequests(uint64_t maxConcurrent) {
  maxConcurrentRequests_ = maxConcurrent;
  maxRequestID_ = maxConcurrent * getRequestIDMultiplier();
}

void MoQSession::validateAndSetVersionFromAlpn(const std::string& alpn) {
  auto version = getVersionFromAlpn(alpn);
  if (version) {
    setVersion(*version);
    negotiatedVersion_ = *version;
  }
}

GroupOrder MoQSession::resolveGroupOrder(
    GroupOrder pubOrder,
    GroupOrder subOrder) {
  if (subOrder != GroupOrder::Default) {
    return subOrder;
  }
  return pubOrder;
}

std::string MoQSession::getMoQTImplementationString() {
  return "moxygen-compat";
}

Subscriber::PublishResult MoQSession::publish(
    PublishRequest pub,
    std::shared_ptr<Publisher::SubscriptionHandle> /*handle*/) {
  return compat::makeUnexpected(PublishError{
      pub.requestID, PublishErrorCode::NOT_SUPPORTED, "Not implemented"});
}

// WebTransport callbacks for std-mode
void MoQSession::onNewUniStream(compat::StreamReadHandle* /*rh*/) {
  // TODO: Implement
}

void MoQSession::onNewBidiStream(compat::BidiStreamHandle* /*bh*/) {
  // TODO: Implement
}

void MoQSession::onDatagram(std::unique_ptr<compat::Payload> /*datagram*/) {
  // TODO: Implement
}

void MoQSession::controlWriteLoop(compat::StreamWriteHandle* /*writeHandle*/) {
  // In std-mode, write is event-driven, not a loop
}

void MoQSession::controlReadLoop(compat::StreamReadHandle* /*readHandle*/) {
  // In std-mode, read is callback-driven, not a loop
}

void MoQSession::unidirectionalReadLoop(
    std::shared_ptr<MoQSession> /*session*/,
    compat::StreamReadHandle* /*readHandle*/) {
  // In std-mode, read is callback-driven, not a loop
}

// Track state accessors
std::shared_ptr<MoQSession::SubscribeTrackReceiveState>
MoQSession::getSubscribeTrackReceiveState(TrackAlias alias) {
  auto it = subTracks_.find(alias);
  if (it != subTracks_.end()) {
    return it->second;
  }
  return nullptr;
}

std::shared_ptr<MoQSession::FetchTrackReceiveState>
MoQSession::getFetchTrackReceiveState(RequestID requestID) {
  auto it = fetches_.find(requestID);
  if (it != fetches_.end()) {
    return it->second;
  }
  return nullptr;
}

void MoQSession::setLogger(const std::shared_ptr<MLogger>& logger) {
  logger_ = logger;
}

std::shared_ptr<MLogger> MoQSession::getLogger() const {
  return logger_;
}

std::optional<uint64_t> MoQSession::getDeliveryTimeoutIfPresent(
    const TrackRequestParameters& /*params*/,
    uint64_t /*version*/) {
  // TODO: Implement parameter lookup
  return std::nullopt;
}

// MoQControlCodec::ControlCallback implementations
void MoQSession::onClientSetup(ClientSetup clientSetup) {
  if (dir_ != MoQControlCodec::Direction::SERVER) {
    LOG(ERROR) << "Received CLIENT_SETUP on client";
    close(SessionCloseErrorCode::PROTOCOL_VIOLATION);
    return;
  }

  if (!serverSetupCallback_) {
    LOG(ERROR) << "No server setup callback";
    close(SessionCloseErrorCode::INTERNAL_ERROR);
    return;
  }

  auto result = serverSetupCallback_->onClientSetup(
      std::move(clientSetup), shared_from_this());

  if (result.hasValue()) {
    auto& serverSetup = result.value();
    negotiatedVersion_ = serverSetup.selectedVersion;
    moqFrameWriter_.initializeVersion(*negotiatedVersion_);

    // Use standalone function, not method
    auto writeRes =
        writeServerSetup(controlWriteBuf_, serverSetup, *negotiatedVersion_);
    if (writeRes.hasError()) {
      LOG(ERROR) << "Failed to write SERVER_SETUP";
      close(SessionCloseErrorCode::INTERNAL_ERROR);
      return;
    }
    setupComplete_ = true;
  } else {
    LOG(ERROR) << "Server setup callback failed";
    close(SessionCloseErrorCode::INTERNAL_ERROR);
  }
}

void MoQSession::onServerSetup(ServerSetup setup) {
  if (dir_ != MoQControlCodec::Direction::CLIENT) {
    LOG(ERROR) << "Received SERVER_SETUP on server";
    close(SessionCloseErrorCode::PROTOCOL_VIOLATION);
    return;
  }

  negotiatedVersion_ = setup.selectedVersion;
  moqFrameWriter_.initializeVersion(*negotiatedVersion_);
  setupComplete_ = true;

  if (setupCallback_) {
    setupCallback_->onSuccess(std::move(setup));
    setupCallback_.reset();
  }
}

void MoQSession::onSubscribe(SubscribeRequest subscribeRequest) {
  if (!publishHandler_) {
    SubscribeError err{
        subscribeRequest.requestID,
        SubscribeErrorCode::NOT_SUPPORTED,
        "No publish handler"};
    subscribeError(err);
    return;
  }
  // TODO: Forward to publish handler
}

void MoQSession::onSubscribeUpdate(SubscribeUpdate /*subscribeUpdate*/) {}
void MoQSession::onSubscribeOk(SubscribeOk /*subscribeOk*/) {}
void MoQSession::onRequestOk(RequestOk /*requestOk*/, FrameType /*frameType*/) {}
void MoQSession::onRequestError(RequestError /*requestError*/, FrameType /*frameType*/) {}
void MoQSession::onUnsubscribe(Unsubscribe /*unsubscribe*/) {}
void MoQSession::onPublish(PublishRequest /*publish*/) {}
void MoQSession::onPublishOk(PublishOk /*publishOk*/) {}
void MoQSession::onSubscribeDone(SubscribeDone /*subscribeDone*/) {}
void MoQSession::onMaxRequestID(MaxRequestID maxSubId) {
  peerMaxRequestID_ = maxSubId.requestID.value;
}
void MoQSession::onRequestsBlocked(RequestsBlocked /*requestsBlocked*/) {}
void MoQSession::onFetch(Fetch /*fetch*/) {}
void MoQSession::onFetchCancel(FetchCancel /*fetchCancel*/) {}
void MoQSession::onFetchOk(FetchOk /*fetchOk*/) {}
void MoQSession::onTrackStatus(TrackStatus /*trackStatus*/) {}
void MoQSession::onTrackStatusOk(TrackStatusOk /*trackStatusOk*/) {}
void MoQSession::onTrackStatusError(TrackStatusError /*trackStatusError*/) {}

void MoQSession::onGoaway(Goaway goway) {
  receivedGoaway_ = true;
  if (subscribeHandler_) {
    subscribeHandler_->goaway(std::move(goway));
  }
}

void MoQSession::onConnectionError(ErrorCode error) {
  LOG(ERROR) << "Connection error: " << static_cast<uint32_t>(error);
  close(SessionCloseErrorCode::INTERNAL_ERROR);
}

void MoQSession::onPublishNamespace(PublishNamespace /*publishNamespace*/) {}
void MoQSession::onPublishNamespaceDone(PublishNamespaceDone /*publishNamespaceDone*/) {}
void MoQSession::onPublishNamespaceCancel(PublishNamespaceCancel /*publishNamespaceCancel*/) {}
void MoQSession::onSubscribeNamespace(SubscribeNamespace /*subscribeNamespace*/) {}
void MoQSession::onUnsubscribeNamespace(UnsubscribeNamespace /*unsubscribeNamespace*/) {}

void MoQSession::removeSubscriptionState(TrackAlias alias, RequestID id) {
  subTracks_.erase(alias);
  reqIdToTrackAlias_.erase(id);
  checkForCloseOnDrain();
}

void MoQSession::checkForCloseOnDrain() {
  if (draining_ && subTracks_.empty() && fetches_.empty() && pubTracks_.empty()) {
    close(SessionCloseErrorCode::NO_ERROR);
  }
}

void MoQSession::sendMaxRequestID(bool /*signalWriteLoop*/) {
  MaxRequestID maxReqId{RequestID(maxRequestID_)};
  auto res = moqFrameWriter_.writeMaxRequestID(controlWriteBuf_, maxReqId);
  if (res.hasError()) {
    LOG(ERROR) << "Failed to write MAX_REQUEST_ID";
  }
}

void MoQSession::fetchComplete(RequestID requestID) {
  fetches_.erase(requestID);
  checkForCloseOnDrain();
}

uint64_t MoQSession::getMaxRequestIDIfPresent(const SetupParameters& /*params*/) {
  return 0;
}

uint64_t MoQSession::getMaxAuthTokenCacheSizeIfPresent(const SetupParameters& /*params*/) {
  return 0;
}

std::optional<std::string> MoQSession::getMoQTImplementationIfPresent(
    const SetupParameters& /*params*/) {
  return std::nullopt;
}

bool MoQSession::shouldIncludeMoqtImplementationParam(
    const std::vector<uint64_t>& /*supportedVersions*/) {
  return true;
}

void MoQSession::setPublisherPriorityFromParams(
    const TrackRequestParameters& /*params*/,
    const std::shared_ptr<TrackPublisherImpl>& /*trackPublisher*/) {}

void MoQSession::setPublisherPriorityFromParams(
    const TrackRequestParameters& /*params*/,
    const std::shared_ptr<SubscribeTrackReceiveState>& /*trackPublisher*/) {}

bool MoQSession::closeSessionIfRequestIDInvalid(
    RequestID /*requestID*/,
    bool /*skipCheck*/,
    bool /*isNewRequest*/,
    bool /*parityMatters*/) {
  return false;
}

void MoQSession::deliverBufferedData(TrackAlias /*trackAlias*/) {}

void MoQSession::aliasifyAuthTokens(
    Parameters& /*params*/,
    const std::optional<uint64_t>& /*forceVersion*/) {}

RequestID MoQSession::getNextRequestID() {
  auto id = nextRequestID_;
  nextRequestID_ += getRequestIDMultiplier();
  return RequestID(id);
}

void MoQSession::retireRequestID(bool signalWriteLoop) {
  closedRequests_++;
  if (closedRequests_ >= maxConcurrentRequests_ / 2) {
    maxRequestID_ += closedRequests_ * getRequestIDMultiplier();
    closedRequests_ = 0;
    sendMaxRequestID(signalWriteLoop);
  }
}

void MoQSession::initializeNegotiatedVersion(uint64_t negotiatedVersion) {
  negotiatedVersion_ = negotiatedVersion;
  moqFrameWriter_.initializeVersion(negotiatedVersion);
  controlCodec_.initializeVersion(negotiatedVersion);
}

void MoQSession::PublisherImpl::fetchComplete() {
  if (session_) {
    session_->fetchComplete(requestID_);
  }
}

// Response helpers
void MoQSession::sendSubscribeOk(const SubscribeOk& subOk) {
  moqFrameWriter_.writeSubscribeOk(controlWriteBuf_, subOk);
}

void MoQSession::subscribeError(const SubscribeError& subErr) {
  moqFrameWriter_.writeRequestError(
      controlWriteBuf_, subErr, FrameType::SUBSCRIBE_ERROR);
}

void MoQSession::unsubscribe(const Unsubscribe& unsub) {
  moqFrameWriter_.writeUnsubscribe(controlWriteBuf_, unsub);
}

void MoQSession::subscribeUpdate(const SubscribeUpdate& subUpdate) {
  moqFrameWriter_.writeSubscribeUpdate(controlWriteBuf_, subUpdate);
}

void MoQSession::subscribeUpdateOk(const RequestOk& requestOk) {
  moqFrameWriter_.writeRequestOk(
      controlWriteBuf_, requestOk, FrameType::REQUEST_OK);
}

void MoQSession::subscribeUpdateError(
    const SubscribeUpdateError& requestError,
    RequestID /*subscriptionRequestID*/) {
  moqFrameWriter_.writeRequestError(
      controlWriteBuf_, requestError, FrameType::REQUEST_ERROR);
}

void MoQSession::sendSubscribeDone(const SubscribeDone& subDone) {
  moqFrameWriter_.writeSubscribeDone(controlWriteBuf_, subDone);
}

void MoQSession::fetchOk(const FetchOk& fetchOkMsg) {
  moqFrameWriter_.writeFetchOk(controlWriteBuf_, fetchOkMsg);
}

void MoQSession::fetchError(const FetchError& fetchErr) {
  moqFrameWriter_.writeRequestError(
      controlWriteBuf_, fetchErr, FrameType::FETCH_ERROR);
}

void MoQSession::fetchCancel(const FetchCancel& fetchCan) {
  moqFrameWriter_.writeFetchCancel(controlWriteBuf_, fetchCan);
}

void MoQSession::publishOk(const PublishOk& pubOk) {
  moqFrameWriter_.writePublishOk(controlWriteBuf_, pubOk);
}

void MoQSession::publishError(const PublishError& pubErr) {
  moqFrameWriter_.writeRequestError(
      controlWriteBuf_, pubErr, FrameType::PUBLISH_ERROR);
}

void MoQSession::trackStatusOk(const TrackStatusOk& tsOk) {
  moqFrameWriter_.writeTrackStatusOk(controlWriteBuf_, tsOk);
}

void MoQSession::trackStatusError(const TrackStatusError& tsErr) {
  moqFrameWriter_.writeTrackStatusError(controlWriteBuf_, tsErr);
}

void MoQSession::publishNamespaceError(
    const PublishNamespaceError& publishNamespaceErr) {
  moqFrameWriter_.writeRequestError(
      controlWriteBuf_, publishNamespaceErr, FrameType::PUBLISH_NAMESPACE_ERROR);
}

void MoQSession::subscribeNamespaceError(
    const SubscribeNamespaceError& subscribeNamespaceErr) {
  moqFrameWriter_.writeRequestError(
      controlWriteBuf_,
      subscribeNamespaceErr,
      FrameType::SUBSCRIBE_NAMESPACE_ERROR);
}

} // namespace moxygen

#endif // !MOXYGEN_USE_FOLLY
