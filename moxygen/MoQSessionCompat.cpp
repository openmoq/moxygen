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
class MoQSessionBase::TrackReceiveStateBase {
 public:
  virtual ~TrackReceiveStateBase() = default;
};

class MoQSessionBase::SubscribeTrackReceiveState : public TrackReceiveStateBase {
 public:
  SubscribeTrackReceiveState() = default;
};

class MoQSessionBase::FetchTrackReceiveState : public TrackReceiveStateBase {
 public:
  FetchTrackReceiveState() = default;
};

// MoQSessionBase implementation

MoQSessionBase::MoQSessionBase(
    MoQControlCodec::Direction dir,
    std::shared_ptr<MoQExecutor> exec,
    ServerSetupCallback* serverSetupCallback)
    : dir_(dir),
      exec_(std::move(exec)),
      serverSetupCallback_(serverSetupCallback),
      controlCodec_(dir, this) {}

MoQSessionBase::~MoQSessionBase() = default;

void MoQSessionBase::cleanup() {
  cancellationSource_.requestCancellation();
  subscribeHandler_.reset();
  publishHandler_.reset();
  subTracks_.clear();
  fetches_.clear();
  pubTracks_.clear();
}

void MoQSessionBase::setServerMaxTokenCacheSizeGuess(size_t size) {
  tokenCache_.setMaxSize(size);
}

void MoQSessionBase::setVersion(uint64_t version) {
  moqFrameWriter_.initializeVersion(version);
}

void MoQSessionBase::setMoqSettings(MoQSettings settings) {
  moqSettings_ = std::move(settings);
}

void MoQSessionBase::setPublishHandler(std::shared_ptr<Publisher> publishHandler) {
  publishHandler_ = std::move(publishHandler);
}

void MoQSessionBase::setSubscribeHandler(
    std::shared_ptr<Subscriber> subscribeHandler) {
  subscribeHandler_ = std::move(subscribeHandler);
}

void MoQSessionBase::goaway(Goaway goway) {
  // This will be implemented by derived classes
}

void MoQSessionBase::setMaxConcurrentRequests(uint64_t maxConcurrent) {
  maxConcurrentRequests_ = maxConcurrent;
  maxRequestID_ = maxConcurrent * getRequestIDMultiplier();
}

void MoQSessionBase::validateAndSetVersionFromAlpn(const std::string& alpn) {
  auto version = getVersionFromAlpn(alpn);
  if (version) {
    setVersion(*version);
    negotiatedVersion_ = *version;
  }
}

GroupOrder MoQSessionBase::resolveGroupOrder(
    GroupOrder pubOrder,
    GroupOrder subOrder) {
  if (subOrder != GroupOrder::Default) {
    return subOrder;
  }
  return pubOrder;
}

std::string MoQSessionBase::getMoQTImplementationString() {
  return "moxygen-compat";
}

void MoQSessionBase::setLogger(const std::shared_ptr<MLogger>& logger) {
  logger_ = logger;
}

std::shared_ptr<MLogger> MoQSessionBase::getLogger() const {
  return logger_;
}

std::optional<uint64_t> MoQSessionBase::getDeliveryTimeoutIfPresent(
    const TrackRequestParameters& /*params*/,
    uint64_t /*version*/) {
  return std::nullopt;
}

std::shared_ptr<MoQSessionBase::SubscribeTrackReceiveState>
MoQSessionBase::getSubscribeTrackReceiveState(TrackAlias alias) {
  auto it = subTracks_.find(alias);
  if (it != subTracks_.end()) {
    return it->second;
  }
  return nullptr;
}

std::shared_ptr<MoQSessionBase::FetchTrackReceiveState>
MoQSessionBase::getFetchTrackReceiveState(RequestID requestID) {
  auto it = fetches_.find(requestID);
  if (it != fetches_.end()) {
    return it->second;
  }
  return nullptr;
}

bool MoQSessionBase::closeSessionIfRequestIDInvalid(
    RequestID /*requestID*/,
    bool /*skipCheck*/,
    bool /*isNewRequest*/,
    bool /*parityMatters*/) {
  return false;
}

void MoQSessionBase::deliverBufferedData(TrackAlias /*trackAlias*/) {}

void MoQSessionBase::aliasifyAuthTokens(
    Parameters& /*params*/,
    const std::optional<uint64_t>& /*forceVersion*/) {}

RequestID MoQSessionBase::getNextRequestID() {
  auto id = nextRequestID_;
  nextRequestID_ += getRequestIDMultiplier();
  return RequestID(id);
}

void MoQSessionBase::retireRequestID(bool signalWriteLoop) {
  closedRequests_++;
  if (closedRequests_ >= maxConcurrentRequests_ / 2) {
    maxRequestID_ += closedRequests_ * getRequestIDMultiplier();
    closedRequests_ = 0;
    sendMaxRequestID(signalWriteLoop);
  }
}

void MoQSessionBase::removeSubscriptionState(TrackAlias alias, RequestID id) {
  subTracks_.erase(alias);
  reqIdToTrackAlias_.erase(id);
  checkForCloseOnDrain();
}

void MoQSessionBase::checkForCloseOnDrain() {
  if (draining_ && subTracks_.empty() && fetches_.empty() && pubTracks_.empty()) {
    close(SessionCloseErrorCode::NO_ERROR);
  }
}

void MoQSessionBase::sendMaxRequestID(bool /*signalWriteLoop*/) {
  // Implemented by derived class
}

void MoQSessionBase::fetchComplete(RequestID requestID) {
  fetches_.erase(requestID);
  checkForCloseOnDrain();
}

void MoQSessionBase::initializeNegotiatedVersion(uint64_t negotiatedVersion) {
  negotiatedVersion_ = negotiatedVersion;
  moqFrameWriter_.initializeVersion(negotiatedVersion);
  controlCodec_.initializeVersion(negotiatedVersion);
}

uint64_t MoQSessionBase::getMaxRequestIDIfPresent(const SetupParameters& /*params*/) {
  return 0;
}

uint64_t MoQSessionBase::getMaxAuthTokenCacheSizeIfPresent(const SetupParameters& /*params*/) {
  return 0;
}

std::optional<std::string> MoQSessionBase::getMoQTImplementationIfPresent(
    const SetupParameters& /*params*/) {
  return std::nullopt;
}

bool MoQSessionBase::shouldIncludeMoqtImplementationParam(
    const std::vector<uint64_t>& /*supportedVersions*/) {
  return true;
}

void MoQSessionBase::PublisherImpl::fetchComplete() {
  if (session_) {
    session_->fetchComplete(requestID_);
  }
}

// Response helpers - base implementations
void MoQSessionBase::sendSubscribeOk(const SubscribeOk& /*subOk*/) {}
void MoQSessionBase::subscribeError(const SubscribeError& /*subErr*/) {}
void MoQSessionBase::unsubscribe(const Unsubscribe& /*unsub*/) {}
void MoQSessionBase::subscribeUpdate(const SubscribeUpdate& /*subUpdate*/) {}
void MoQSessionBase::subscribeUpdateOk(const RequestOk& /*requestOk*/) {}
void MoQSessionBase::subscribeUpdateError(
    const SubscribeUpdateError& /*requestError*/,
    RequestID /*subscriptionRequestID*/) {}
void MoQSessionBase::sendSubscribeDone(const SubscribeDone& /*subDone*/) {}
void MoQSessionBase::fetchOk(const FetchOk& /*fetchOkMsg*/) {}
void MoQSessionBase::fetchError(const FetchError& /*fetchErr*/) {}
void MoQSessionBase::fetchCancel(const FetchCancel& /*fetchCan*/) {}
void MoQSessionBase::publishOk(const PublishOk& /*pubOk*/) {}
void MoQSessionBase::publishError(const PublishError& /*pubErr*/) {}
void MoQSessionBase::trackStatusOk(const TrackStatusOk& /*tsOk*/) {}
void MoQSessionBase::trackStatusError(const TrackStatusError& /*tsErr*/) {}
void MoQSessionBase::publishNamespaceError(
    const PublishNamespaceError& /*publishNamespaceErr*/) {}
void MoQSessionBase::subscribeNamespaceError(
    const SubscribeNamespaceError& /*subscribeNamespaceErr*/) {}


// MoQSession (std-mode) implementation

MoQSession::MoQSession(
    std::shared_ptr<compat::WebTransportInterface> wt,
    std::shared_ptr<MoQExecutor> exec)
    : MoQSessionBase(MoQControlCodec::Direction::CLIENT, std::move(exec)),
      wt_(std::move(wt)) {}

MoQSession::MoQSession(
    std::shared_ptr<compat::WebTransportInterface> wt,
    ServerSetupCallback& serverSetupCallback,
    std::shared_ptr<MoQExecutor> exec)
    : MoQSessionBase(
          MoQControlCodec::Direction::SERVER,
          std::move(exec),
          &serverSetupCallback),
      wt_(std::move(wt)) {}

MoQSession::~MoQSession() {
  cleanup();
}

void MoQSession::cleanup() {
  MoQSessionBase::cleanup();
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

void MoQSession::setupWithCallback(
    ClientSetup setup,
    std::shared_ptr<compat::ResultCallback<ServerSetup, SessionCloseErrorCode>>
        callback) {
  setupCallback_ = std::move(callback);

  auto res = writeClientSetup(
      controlWriteBuf_, setup, setup.supportedVersions[0]);
  if (res.hasError()) {
    if (setupCallback_) {
      setupCallback_->onError(SessionCloseErrorCode::INTERNAL_ERROR);
      setupCallback_.reset();
    }
  }
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
    moqFrameWriter_.writeRequestError(
        controlWriteBuf_, err, FrameType::SUBSCRIBE_ERROR);
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

} // namespace moxygen

#endif // !MOXYGEN_USE_FOLLY
