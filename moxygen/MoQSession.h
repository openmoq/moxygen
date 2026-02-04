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
  // Forward declarations for nested classes (private access)
  class PendingRequestState;
  class ReceiverFetchHandle;

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
  void goaway(Goaway goaway) override;
  void setLogger(const std::shared_ptr<MLogger>& logger) override;
  std::shared_ptr<MLogger> getLogger() const override;
  void deliverBufferedData(TrackAlias trackAlias) override;
  void trackStatusOk(const TrackStatusOk& trackStatusOk) override;
  void trackStatusError(const TrackStatusError& trackStatusError) override;
  void removeSubscriptionState(TrackAlias alias, RequestID id) override;
  RequestID getNextRequestID() override;
  void retireRequestID(bool signalWriteLoop) override;
  void publishNamespaceError(const PublishNamespaceError& e) override;
  void subscribeNamespaceError(const SubscribeNamespaceError& e) override;
  void sendSubscribeOk(const SubscribeOk& subOk) override;
  void subscribeError(const SubscribeError& subErr) override;
  void unsubscribe(const Unsubscribe& unsubscribe) override;
  void subscribeUpdate(const SubscribeUpdate& subUpdate) override;
  void subscribeUpdateOk(const RequestOk& requestOk) override;
  void subscribeUpdateError(
      const SubscribeUpdateError& requestError,
      RequestID subscriptionRequestID) override;
  void sendSubscribeDone(const SubscribeDone& subDone) override;
  void fetchOk(const FetchOk& fetchOk) override;
  void fetchError(const FetchError& fetchError) override;
  void fetchCancel(const FetchCancel& fetchCancel) override;
  void publishOk(const PublishOk& pubOk) override;
  void publishError(const PublishError& publishError) override;
  void checkForCloseOnDrain() override;
  void sendMaxRequestID(bool signalWriteLoop) override;
  void fetchComplete(RequestID requestID) override;
  void initializeNegotiatedVersion(uint64_t negotiatedVersion) override;

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
    XLOG(INFO) << __func__ << "err="
              << (err ? folly::to<std::string>(*err) : std::string("none"))
              << " sess=" << this;
    close(SessionCloseErrorCode::NO_ERROR);
  }
  void onSessionDrain() noexcept override {
    XLOG(INFO) << __func__ << " sess=" << this;
  }

  // Bring base class nested types into MoQSession scope
  using PublisherImpl = MoQSessionBase::PublisherImpl;

  // Extended PublisherImpl with transport access
  class TrackPublisherImpl;
  class FetchPublisherImpl;
  class ReceiverSubscriptionHandle;

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
  // Forward declare iterator type - full definition after PendingRequestState class
  void handleSubscribeUpdateOkFromRequestOk(
      const RequestOk& requestOk,
      folly::F14FastMap<
          RequestID,
          std::unique_ptr<PendingRequestState>,
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
  class PendingRequestState {
   public:
    enum class Type : uint8_t {
      SUBSCRIBE_TRACK,
      PUBLISH,
      TRACK_STATUS,
      FETCH,
      SUBSCRIBE_UPDATE,
      // PublishNamespace types - only handled by MoQRelaySession subclass
      PUBLISH_NAMESPACE,
      SUBSCRIBE_NAMESPACE
    };

    // Make polymorphic for subclassing - destructor implemented below

   protected:
    Type type_;
    union Storage {
      Storage() {}
      ~Storage() {}

      std::shared_ptr<SubscribeTrackReceiveState> subscribeTrack_;
      folly::coro::Promise<folly::Expected<PublishOk, PublishError>> publish_;
      folly::coro::Promise<folly::Expected<TrackStatusOk, TrackStatusError>>
          trackStatus_;
      std::shared_ptr<FetchTrackReceiveState> fetchTrack_;
      folly::coro::Promise<
          folly::Expected<SubscribeUpdateOk, SubscribeUpdateError>>
          subscribeUpdate_;
    } storage_;

   public:
    // Factory methods for type-safe construction returning unique_ptr
    static std::unique_ptr<PendingRequestState> makeSubscribeTrack(
        std::shared_ptr<SubscribeTrackReceiveState> state) {
      auto result = std::make_unique<PendingRequestState>();
      result->type_ = Type::SUBSCRIBE_TRACK;
      new (&result->storage_.subscribeTrack_) auto(std::move(state));
      return result;
    }

    static std::unique_ptr<PendingRequestState> makePublish(
        folly::coro::Promise<folly::Expected<PublishOk, PublishError>>
            promise) {
      auto result = std::make_unique<PendingRequestState>();
      result->type_ = Type::PUBLISH;
      new (&result->storage_.publish_) auto(std::move(promise));
      return result;
    }

    static std::unique_ptr<PendingRequestState> makeTrackStatus(
        folly::coro::Promise<folly::Expected<TrackStatusOk, TrackStatusError>>
            promise) {
      auto result = std::make_unique<PendingRequestState>();
      result->type_ = Type::TRACK_STATUS;
      new (&result->storage_.trackStatus_) auto(std::move(promise));
      return result;
    }

    static std::unique_ptr<PendingRequestState> makeFetch(
        std::shared_ptr<FetchTrackReceiveState> state) {
      auto result = std::make_unique<PendingRequestState>();
      result->type_ = Type::FETCH;
      new (&result->storage_.fetchTrack_) auto(std::move(state));
      return result;
    }

    static std::unique_ptr<PendingRequestState> makeSubscribeUpdate(
        folly::coro::Promise<
            folly::Expected<SubscribeUpdateOk, SubscribeUpdateError>> promise) {
      auto result = std::make_unique<PendingRequestState>();
      result->type_ = Type::SUBSCRIBE_UPDATE;
      new (&result->storage_.subscribeUpdate_) auto(std::move(promise));
      return result;
    }

    // Delete copy/move operations as this is held in unique_ptr
    PendingRequestState(const PendingRequestState&) = delete;
    PendingRequestState(PendingRequestState&&) = delete;
    PendingRequestState& operator=(const PendingRequestState&) = delete;
    PendingRequestState& operator=(PendingRequestState&&) = delete;

    // Virtual destructor implementation
    virtual ~PendingRequestState() {
      switch (type_) {
        case Type::SUBSCRIBE_TRACK:
          storage_.subscribeTrack_.~shared_ptr();
          break;
        case Type::PUBLISH:
          storage_.publish_.~Promise();
          break;
        case Type::TRACK_STATUS:
          storage_.trackStatus_.~Promise();
          break;
        case Type::FETCH:
          storage_.fetchTrack_.~shared_ptr<FetchTrackReceiveState>();
          break;
        case Type::SUBSCRIBE_UPDATE:
          storage_.subscribeUpdate_.~Promise();
          break;
        case Type::PUBLISH_NAMESPACE:
        case Type::SUBSCRIBE_NAMESPACE:
          // These types are handled by MoQRelaySession subclass destructor
          break;
      }
    }

    // Duck typing access - overloaded functions for each type
    std::shared_ptr<SubscribeTrackReceiveState>* tryGetSubscribeTrack() {
      return type_ == Type::SUBSCRIBE_TRACK ? &storage_.subscribeTrack_
                                            : nullptr;
    }

    FrameType getFrameType(bool ok) const {
      switch (type_) {
        case Type::SUBSCRIBE_TRACK:
          return ok ? FrameType::SUBSCRIBE_OK : FrameType::SUBSCRIBE_ERROR;
        case Type::PUBLISH:
          return ok ? FrameType::PUBLISH_OK : FrameType::PUBLISH_ERROR;
        case Type::TRACK_STATUS:
          return ok ? FrameType::TRACK_STATUS_OK : FrameType::TRACK_STATUS;
        case Type::FETCH:
          return ok ? FrameType::FETCH_OK : FrameType::FETCH_ERROR;
        case Type::SUBSCRIBE_UPDATE:
          return ok ? FrameType::REQUEST_OK : FrameType::REQUEST_ERROR;
        case Type::PUBLISH_NAMESPACE:
          return ok ? FrameType::PUBLISH_NAMESPACE_OK
                    : FrameType::PUBLISH_NAMESPACE_ERROR;
        case Type::SUBSCRIBE_NAMESPACE:
          return ok ? FrameType::SUBSCRIBE_NAMESPACE_OK
                    : FrameType::SUBSCRIBE_NAMESPACE_ERROR;
      }
      folly::assume_unreachable();
    }
    FrameType getOkFrameType() const {
      return getFrameType(true);
    }

    FrameType getErrorFrameType() const {
      return getFrameType(false);
    }

    virtual folly::Expected<Type, folly::Unit> setError(
        RequestError error,
        FrameType frameType);

    const std::shared_ptr<SubscribeTrackReceiveState>* tryGetSubscribeTrack()
        const {
      return type_ == Type::SUBSCRIBE_TRACK ? &storage_.subscribeTrack_
                                            : nullptr;
    }

    folly::coro::Promise<folly::Expected<PublishOk, PublishError>>*
    tryGetPublish() {
      return type_ == Type::PUBLISH ? &storage_.publish_ : nullptr;
    }

    folly::coro::Promise<folly::Expected<TrackStatusOk, TrackStatusError>>*
    tryGetTrackStatus() {
      return type_ == Type::TRACK_STATUS ? &storage_.trackStatus_ : nullptr;
    }

    std::shared_ptr<FetchTrackReceiveState>* tryGetFetch() {
      return type_ == Type::FETCH ? &storage_.fetchTrack_ : nullptr;
    }

    folly::coro::Promise<
        folly::Expected<SubscribeUpdateOk, SubscribeUpdateError>>*
    tryGetSubscribeUpdate() {
      return type_ == Type::SUBSCRIBE_UPDATE ? &storage_.subscribeUpdate_
                                             : nullptr;
    }

    Type getType() const {
      return type_;
    }

   public:
    // Default constructor - only use via factory methods
    PendingRequestState() = default;
  };
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
    XLOG(INFO) << __func__ << "err="
              << (err ? std::to_string(*err) : std::string("none"))
              << " sess=" << this;
    close(SessionCloseErrorCode::NO_ERROR);
  }
  void onSessionDrain() {
    XLOG(INFO) << __func__ << " sess=" << this;
  }

  compat::WebTransportInterface* getWebTransport() const {
    return wt_.get();
  }

 protected:
  // Protected for MoQRelaySession subclass access
  void cleanup() override;

  // MoQControlCodec::ControlCallback overrides - protected for subclass override
  void onRequestOk(RequestOk requestOk, FrameType frameType) override;

  // PublishNamespace callbacks - protected for subclass override
  void onPublishNamespace(PublishNamespace publishNamespace) override;
  void onPublishNamespaceDone(PublishNamespaceDone publishNamespaceDone) override;
  void onPublishNamespaceCancel(PublishNamespaceCancel publishNamespaceCancel) override;
  void onSubscribeNamespace(SubscribeNamespace subscribeNamespace) override;
  void onUnsubscribeNamespace(UnsubscribeNamespace unsubscribeNamespace) override;

  // Protected state for subclass access
  std::shared_ptr<compat::WebTransportInterface> wt_;
  compat::ByteBufferQueue controlWriteBuf_;

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

  std::shared_ptr<compat::ResultCallback<ServerSetup, SessionCloseErrorCode>>
      setupCallback_;
};

#endif // MOXYGEN_USE_FOLLY

} // namespace moxygen
