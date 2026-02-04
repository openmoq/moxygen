/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <moxygen/MoQSessionBase.h>

#if MOXYGEN_USE_FOLLY
#include <folly/MaybeManagedPtr.h>
#include <folly/coro/Promise.h>
#include <folly/coro/Task.h>
#include <folly/logging/xlog.h>
#include <proxygen/lib/http/webtransport/WebTransport.h>
#include "moxygen/mlog/MLogger.h"
#include "moxygen/util/TimedBaton.h"
#endif

namespace moxygen {

#if MOXYGEN_USE_FOLLY

/**
 * MoQSession - Folly-based MoQ session implementation
 *
 * Uses folly coroutines for async operations and proxygen::WebTransport
 * for the transport layer.
 */
class MoQSession : public MoQSessionBase,
                   public proxygen::WebTransportHandler,
                   public std::enable_shared_from_this<MoQSession> {
 public:
  struct MoQSessionRequestData : public folly::RequestData {
    explicit MoQSessionRequestData(std::shared_ptr<MoQSession> s)
        : session(std::move(s)) {}
    bool hasCallback() override {
      return false;
    }
    std::shared_ptr<MoQSession> session;
  };

  static std::shared_ptr<MoQSession> getRequestSession();

  explicit MoQSession(
      folly::MaybeManagedPtr<proxygen::WebTransport> wt,
      std::shared_ptr<MoQExecutor> exec);

  explicit MoQSession(
      folly::MaybeManagedPtr<proxygen::WebTransport> wt,
      ServerSetupCallback& serverSetupCallback,
      std::shared_ptr<MoQExecutor> exec);

  ~MoQSession() override;

  compat::SocketAddress getPeerAddress() const {
    if (wt_) {
      return wt_->getPeerAddress();
    }
    return compat::SocketAddress();
  }

  [[nodiscard]] quic::TransportInfo getTransportInfo() const {
    if (!wt_) {
      return {};
    }

    // Rate limit getTransportInfo calls to at most once per second
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - lastTransportInfoUpdate_);

    if (elapsed >= std::chrono::seconds(1)) {
      cachedTransportInfo_ = wt_->getTransportInfo();
      lastTransportInfoUpdate_ = now;
    }

    return cachedTransportInfo_;
  }

  // MoQSessionBase overrides
  void start() override;
  void drain() override;
  void close(SessionCloseErrorCode error) override;

  // Async setup using coroutines
  compat::Task<ServerSetup> setup(ClientSetup setup);

  // Subscriber interface - publish method
  Subscriber::PublishResult publish(
      PublishRequest pub,
      std::shared_ptr<Publisher::SubscriptionHandle> handle = nullptr) override;

  // Publisher interface - coroutine methods
  compat::Task<TrackStatusResult> trackStatus(
      TrackStatus trackStatus) override;

  compat::Task<SubscribeResult> subscribe(
      SubscribeRequest sub,
      std::shared_ptr<TrackConsumer> callback) override;

  compat::Task<FetchResult> fetch(
      Fetch fetch,
      std::shared_ptr<FetchConsumer> fetchy) override;

  struct JoinResult {
    SubscribeResult subscribeResult;
    FetchResult fetchResult;
  };
  compat::Task<JoinResult> join(
      SubscribeRequest subscribe,
      std::shared_ptr<TrackConsumer> subscribeCallback,
      uint64_t joiningStart,
      uint8_t fetchPri,
      GroupOrder fetchOrder,
      std::vector<Parameter> fetchParams,
      std::shared_ptr<FetchConsumer> fetchCallback,
      FetchType fetchType);

  // WebTransportHandler overrides
  void onNewUniStream(
      proxygen::WebTransport::StreamReadHandle* rh) noexcept override;
  void onNewBidiStream(
      proxygen::WebTransport::BidiStreamHandle bh) noexcept override;
  void onDatagram(std::unique_ptr<folly::IOBuf> datagram) noexcept override;
  void onSessionEnd(folly::Optional<uint32_t> err) noexcept override {
    LOG(INFO) << __func__ << "err="
              << (err ? folly::to<std::string>(*err) : std::string("none"))
              << " sess=" << this;
    close(SessionCloseErrorCode::NO_ERROR);
  }
  void onSessionDrain() noexcept override {
    LOG(INFO) << __func__ << " sess=" << this;
  }

  // Extended PublisherImpl with transport access
  class TrackPublisherImpl;
  class FetchPublisherImpl;

  proxygen::WebTransport* getWebTransport() const {
    return wt_.get();
  }

 private:
  static const folly::RequestToken& sessionRequestToken();

  compat::Task<void> controlWriteLoop(
      proxygen::WebTransport::StreamWriteHandle* writeHandle);
  compat::Task<void> controlReadLoop(
      proxygen::WebTransport::StreamReadHandle* readHandle);

  compat::Task<void> unidirectionalReadLoop(
      std::shared_ptr<MoQSession> session,
      proxygen::WebTransport::StreamReadHandle* readHandle);

  compat::Task<void> handleTrackStatus(TrackStatus trackStatus);
  compat::Task<void> handleSubscribe(
      SubscribeRequest sub,
      std::shared_ptr<TrackPublisherImpl> trackPublisher);
  compat::Task<void> handleFetch(
      Fetch fetch,
      std::shared_ptr<FetchPublisherImpl> fetchPublisher);
  compat::Task<void> handlePublish(
      PublishRequest publish,
      std::shared_ptr<Publisher::SubscriptionHandle> publishHandle);

  // MoQControlCodec::ControlCallback overrides
  void onClientSetup(ClientSetup clientSetup) override;
  void onServerSetup(ServerSetup setup) override;
  void onSubscribe(SubscribeRequest subscribeRequest) override;
  void onSubscribeUpdate(SubscribeUpdate subscribeUpdate) override;
  void onSubscribeOk(SubscribeOk subscribeOk) override;
  void onRequestOk(RequestOk requestOk, FrameType frameType) override;
  void onRequestError(RequestError requestError, FrameType frameType) override;
  void onUnsubscribe(Unsubscribe unsubscribe) override;
  void onPublish(PublishRequest publish) override;
  void onPublishOk(PublishOk publishOk) override;
  void onSubscribeDone(SubscribeDone subscribeDone) override;
  void onMaxRequestID(MaxRequestID maxSubId) override;
  void onRequestsBlocked(RequestsBlocked requestsBlocked) override;
  void onFetch(Fetch fetch) override;
  void onFetchCancel(FetchCancel fetchCancel) override;
  void onFetchOk(FetchOk fetchOk) override;
  void onTrackStatus(TrackStatus trackStatus) override;
  void onTrackStatusOk(TrackStatusOk trackStatusOk) override;
  void onTrackStatusError(TrackStatusError trackStatusError) override;
  void onGoaway(Goaway goaway) override;
  void onConnectionError(ErrorCode error) override;

  // PublishNamespace callbacks - default implementations
  void onPublishNamespace(PublishNamespace publishNamespace) override;
  void onPublishNamespaceDone(PublishNamespaceDone publishNamespaceDone) override;
  void onPublishNamespaceCancel(PublishNamespaceCancel publishNamespaceCancel) override;
  void onSubscribeNamespace(SubscribeNamespace subscribeNamespace) override;
  void onUnsubscribeNamespace(UnsubscribeNamespace unsubscribeNamespace) override;

  void setRequestSession() {
    folly::RequestContext::get()->setContextData(
        sessionRequestToken(),
        std::make_unique<MoQSessionRequestData>(shared_from_this()));
  }

  void cleanup() override;
  void removeBufferedSubgroupBaton(TrackAlias alias, TimedBaton* baton);

  void setPublisherPriorityFromParams(
      const TrackRequestParameters& params,
      const std::shared_ptr<TrackPublisherImpl>& trackPublisher);
  void setPublisherPriorityFromParams(
      const TrackRequestParameters& params,
      const std::shared_ptr<SubscribeTrackReceiveState>& trackPublisher);

  void handleTrackStatusOkFromRequestOk(const RequestOk& requestOk);
  void handleSubscribeUpdateOkFromRequestOk(
      const RequestOk& requestOk,
      folly::F14FastMap<
          RequestID,
          std::unique_ptr<class PendingRequestState>,
          RequestID::hash>::iterator reqIt);

  // Folly-specific state
  folly::MaybeManagedPtr<proxygen::WebTransport> wt_;
  folly::IOBufQueue controlWriteBuf_{folly::IOBufQueue::cacheChainLength()};
  moxygen::TimedBaton controlWriteEvent_;

  compat::FastMap<TrackAlias, std::list<TimedBaton*>, TrackAlias::hash>
      bufferedSubgroups_;
  std::list<std::shared_ptr<moxygen::TimedBaton>> subgroupsWaitingForVersion_;

  folly::coro::Promise<ServerSetup> setupPromise_;

  // Cached transport info
  mutable quic::TransportInfo cachedTransportInfo_;
  mutable std::chrono::steady_clock::time_point lastTransportInfoUpdate_{};

  // Pending request state management
  class PendingRequestState;
  folly::F14FastMap<
      RequestID,
      std::unique_ptr<PendingRequestState>,
      RequestID::hash>
      pendingRequests_;

 protected:
  // For MoQRelaySession subclass access
  using PendingRequestIterator = folly::F14FastMap<
      RequestID,
      std::unique_ptr<PendingRequestState>,
      RequestID::hash>::iterator;

  friend class MoQRelaySession;
};

#else // !MOXYGEN_USE_FOLLY

/**
 * MoQSession - Std-mode MoQ session implementation
 *
 * Uses callbacks for async operations and compat::WebTransportInterface
 * for the transport layer.
 */
class MoQSession : public MoQSessionBase,
                   public std::enable_shared_from_this<MoQSession> {
 public:
  explicit MoQSession(
      std::shared_ptr<compat::WebTransportInterface> wt,
      std::shared_ptr<MoQExecutor> exec);

  explicit MoQSession(
      std::shared_ptr<compat::WebTransportInterface> wt,
      ServerSetupCallback& serverSetupCallback,
      std::shared_ptr<MoQExecutor> exec);

  ~MoQSession() override;

  compat::SocketAddress getPeerAddress() const {
    if (wt_) {
      return wt_->getPeerAddress();
    }
    return compat::SocketAddress();
  }

  // MoQSessionBase overrides
  void start() override;
  void drain() override;
  void close(SessionCloseErrorCode error) override;

  // Callback-based setup
  void setupWithCallback(
      ClientSetup setup,
      std::shared_ptr<compat::ResultCallback<ServerSetup, SessionCloseErrorCode>>
          callback);

  // Subscriber interface - publish method
  Subscriber::PublishResult publish(
      PublishRequest pub,
      std::shared_ptr<Publisher::SubscriptionHandle> handle = nullptr) override;

  // Transport event handlers
  void onNewUniStream(compat::StreamReadHandle* rh);
  void onNewBidiStream(compat::BidiStreamHandle* bh);
  void onDatagram(std::unique_ptr<compat::Payload> datagram);
  void onSessionEnd(std::optional<uint32_t> err) {
    LOG(INFO) << __func__ << "err="
              << (err ? std::to_string(*err) : std::string("none"))
              << " sess=" << this;
    close(SessionCloseErrorCode::NO_ERROR);
  }
  void onSessionDrain() {
    LOG(INFO) << __func__ << " sess=" << this;
  }

  compat::WebTransportInterface* getWebTransport() const {
    return wt_.get();
  }

 private:
  void controlWriteLoop(compat::StreamWriteHandle* writeHandle);
  void controlReadLoop(compat::StreamReadHandle* readHandle);

  void unidirectionalReadLoop(
      std::shared_ptr<MoQSession> session,
      compat::StreamReadHandle* readHandle);

  // MoQControlCodec::ControlCallback overrides
  void onClientSetup(ClientSetup clientSetup) override;
  void onServerSetup(ServerSetup setup) override;
  void onSubscribe(SubscribeRequest subscribeRequest) override;
  void onSubscribeUpdate(SubscribeUpdate subscribeUpdate) override;
  void onSubscribeOk(SubscribeOk subscribeOk) override;
  void onRequestOk(RequestOk requestOk, FrameType frameType) override;
  void onRequestError(RequestError requestError, FrameType frameType) override;
  void onUnsubscribe(Unsubscribe unsubscribe) override;
  void onPublish(PublishRequest publish) override;
  void onPublishOk(PublishOk publishOk) override;
  void onSubscribeDone(SubscribeDone subscribeDone) override;
  void onMaxRequestID(MaxRequestID maxSubId) override;
  void onRequestsBlocked(RequestsBlocked requestsBlocked) override;
  void onFetch(Fetch fetch) override;
  void onFetchCancel(FetchCancel fetchCancel) override;
  void onFetchOk(FetchOk fetchOk) override;
  void onTrackStatus(TrackStatus trackStatus) override;
  void onTrackStatusOk(TrackStatusOk trackStatusOk) override;
  void onTrackStatusError(TrackStatusError trackStatusError) override;
  void onGoaway(Goaway goaway) override;
  void onConnectionError(ErrorCode error) override;

  // PublishNamespace callbacks - default implementations
  void onPublishNamespace(PublishNamespace publishNamespace) override;
  void onPublishNamespaceDone(PublishNamespaceDone publishNamespaceDone) override;
  void onPublishNamespaceCancel(PublishNamespaceCancel publishNamespaceCancel) override;
  void onSubscribeNamespace(SubscribeNamespace subscribeNamespace) override;
  void onUnsubscribeNamespace(UnsubscribeNamespace unsubscribeNamespace) override;

  void cleanup() override;

  // Std-mode specific state
  std::shared_ptr<compat::WebTransportInterface> wt_;
  compat::ByteBufferQueue controlWriteBuf_;

  std::shared_ptr<compat::ResultCallback<ServerSetup, SessionCloseErrorCode>>
      setupCallback_;
};

#endif // MOXYGEN_USE_FOLLY

} // namespace moxygen
