/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <moxygen/MoQCodec.h>
#include <moxygen/MoQConsumers.h>
#include <moxygen/Publisher.h>
#include <moxygen/Subscriber.h>
#include <moxygen/compat/Callbacks.h>
#include <moxygen/compat/Cancellation.h>
#include <moxygen/compat/Containers.h>
#include <moxygen/compat/Debug.h>
#include <moxygen/compat/SocketAddress.h>
#include <moxygen/compat/Try.h>
#include <moxygen/compat/WebTransportInterface.h>
#include <moxygen/events/MoQDeliveryTimer.h>
#include <moxygen/events/MoQExecutor.h>
#include <moxygen/stats/MoQStats.h>

#include <chrono>
#include <memory>
#include <optional>

namespace moxygen {

// Forward declaration for MLogger
class MLogger;

namespace detail {
class ObjectStreamCallback;
} // namespace detail

struct BufferingThresholds {
  // A value of 0 means no threshold
  uint64_t perSubscription{0};
  uint64_t perSession{0}; // Currently unused
};

struct MoQSettings {
  BufferingThresholds bufferingThresholds{};
  // Timeout for waiting for setup to complete
  std::chrono::milliseconds setupTimeout{std::chrono::seconds(5)};
  // Timeout for waiting for version negotiation to complete
  std::chrono::milliseconds versionNegotiationTimeout{std::chrono::seconds(2)};
  // Timeout for waiting for unknown alias resolution
  std::chrono::milliseconds unknownAliasTimeout{std::chrono::seconds(2)};
  // Timeout for waiting for in-flight streams when SUBSCRIBE_DONE is received
  std::chrono::milliseconds publishDoneStreamCountTimeout{
      std::chrono::seconds(2)};
};

/**
 * MoQSessionBase - Common base class for MoQSession implementations
 *
 * This class contains:
 * - Common types and nested classes
 * - Codec callback handling
 * - Shared state management
 * - Non-async utility methods
 *
 * Derived classes (MoQSession for Folly, MoQSessionCompat for std) provide:
 * - Transport-specific members
 * - Async method implementations (coroutines vs callbacks)
 */
class MoQSessionBase : public Subscriber,
                       public Publisher,
                       public MoQControlCodec::ControlCallback {
 public:
  // Executor pointer type - KeepAlive in Folly+mvfst mode, shared_ptr otherwise
#if MOXYGEN_USE_FOLLY && MOXYGEN_QUIC_MVFST
  using ExecutorPtr = MoQExecutor::KeepAlive;
#else
  using ExecutorPtr = std::shared_ptr<MoQExecutor>;
#endif

  class ServerSetupCallback {
   public:
    virtual ~ServerSetupCallback() = default;
    virtual compat::Try<ServerSetup> onClientSetup(
        ClientSetup clientSetup,
        const std::shared_ptr<MoQSessionBase>& session) = 0;

    // Authority validation callback - returns error code if validation fails
    virtual compat::Expected<compat::Unit, SessionCloseErrorCode>
    validateAuthority(
        const ClientSetup& clientSetup,
        uint64_t negotiatedVersion,
        std::shared_ptr<MoQSessionBase> session) = 0;
  };

  class MoQSessionCloseCallback {
   public:
    virtual ~MoQSessionCloseCallback() = default;
    virtual void onMoQSessionClosed() {
      XLOG(INFO) << __func__ << " sess=" << this;
    }
  };

  void setSessionCloseCallback(MoQSessionCloseCallback* cb) {
    closeCallback_ = cb;
  }

  void setServerMaxTokenCacheSizeGuess(size_t size);

  void setVersion(uint64_t version);
  void setMoqSettings(MoQSettings settings);

  const MoQSettings& getMoqSettings() const {
    return moqSettings_;
  }

  void setPublishHandler(std::shared_ptr<Publisher> publishHandler);
  void setSubscribeHandler(std::shared_ptr<Subscriber> subscribeHandler);

  virtual std::optional<uint64_t> getNegotiatedVersion() const {
    return negotiatedVersion_;
  }

  [[nodiscard]] MoQExecutor* getExecutor() const {
    return exec_.get();
  }

  compat::CancellationToken getCancelToken() const {
    return cancellationSource_.getToken();
  }

  compat::CancellationSource& getCancellationSource() {
    return cancellationSource_;
  }

  // Access to publisher tracks map (for nested classes that need to access)
  auto& getPubTracks() {
    return pubTracks_;
  }

  uint64_t maxRequestID() const {
    return maxRequestID_;
  }

  ~MoQSessionBase() override;

  virtual void start() = 0;
  virtual void drain() = 0;
  virtual void close(SessionCloseErrorCode error) = 0;

  virtual void goaway(Goaway goaway) override;

  void setMaxConcurrentRequests(uint64_t maxConcurrent);

  void validateAndSetVersionFromAlpn(const std::string& alpn);

  static GroupOrder resolveGroupOrder(GroupOrder pubOrder, GroupOrder subOrder);

  static std::string getMoQTImplementationString();

  void setPublisherStatsCallback(
      std::shared_ptr<MoQPublisherStatsCallback> publisherStatsCallback) {
    publisherStatsCallback_ = publisherStatsCallback;
  }

  void setSubscriberStatsCallback(
      std::shared_ptr<MoQSubscriberStatsCallback> subscriberStatsCallback) {
    subscriberStatsCallback_ = subscriberStatsCallback;
  }

  void onSubscriptionStreamOpenedByPeer() {
    MOQ_SUBSCRIBER_STATS(subscriberStatsCallback_, onSubscriptionStreamOpened);
  }

  void onSubscriptionStreamClosedByPeer() {
    MOQ_SUBSCRIBER_STATS(subscriberStatsCallback_, onSubscriptionStreamClosed);
  }

  void onSubscriptionStreamOpened() {
    MOQ_PUBLISHER_STATS(publisherStatsCallback_, onSubscriptionStreamOpened);
  }

  void onSubscriptionStreamClosed() {
    MOQ_PUBLISHER_STATS(publisherStatsCallback_, onSubscriptionStreamClosed);
  }

  virtual void setLogger(const std::shared_ptr<MLogger>& logger);
  virtual std::shared_ptr<MLogger> getLogger() const;

  RequestID peekNextRequestID() const {
    return nextRequestID_;
  }

  // Making this public temporarily until we have param management in a single
  // place
  static std::optional<uint64_t> getDeliveryTimeoutIfPresent(
      const TrackRequestParameters& params,
      uint64_t version);

  /**
   * PublisherImpl - Base class for track/fetch publishers
   *
   * This is common to both Folly and std-mode implementations.
   * Transport access is via virtual method to avoid #ifdef in the header.
   */
  class PublisherImpl : public std::enable_shared_from_this<PublisherImpl> {
   public:
    PublisherImpl(
        MoQSessionBase* session,
        FullTrackName ftn,
        RequestID requestID,
        Priority subPriority,
        GroupOrder groupOrder,
        uint64_t version,
        uint64_t bytesBufferedThreshold)
        : session_(session),
          fullTrackName_(std::move(ftn)),
          requestID_(requestID),
          subPriority_(subPriority),
          groupOrder_(groupOrder),
          version_(version),
          bytesBufferedThreshold_(bytesBufferedThreshold) {
      moqFrameWriter_.initializeVersion(version);
    }

    virtual ~PublisherImpl() = default;

    const FullTrackName& fullTrackName() const {
      return fullTrackName_;
    }
    RequestID requestID() const {
      return requestID_;
    }
    uint8_t subPriority() const {
      return subPriority_;
    }
    void setSubPriority(uint8_t subPriority) {
      subPriority_ = subPriority;
    }
    void setGroupOrder(GroupOrder groupOrder) {
      groupOrder_ = groupOrder;
    }

    GroupOrder getGroupOrder() const {
      return groupOrder_;
    }

    std::optional<uint8_t> getPublisherPriority() const {
      return publisherPriority_;
    }

    void setPublisherPriority(uint8_t priority) {
      publisherPriority_ = priority;
    }

    void setSession(MoQSessionBase* session) {
      session_ = session;
    }

    virtual void terminatePublish(
        PublishDone pubDone,
        ResetStreamErrorCode error = ResetStreamErrorCode::INTERNAL_ERROR) = 0;

    virtual void onStreamCreated() {}

    virtual void onStreamComplete(const ObjectHeader& finalHeader) = 0;

    virtual void onTooManyBytesBuffered() = 0;

    bool canBufferBytes(uint64_t numBytes) {
      if (bytesBufferedThreshold_ == 0) {
        return true;
      }
      return (bytesBuffered_ + numBytes <= bytesBufferedThreshold_);
    }

    void fetchComplete();

    uint64_t getVersion() const {
      return version_;
    }

    void onBytesBuffered(uint64_t amount) {
      bytesBuffered_ += amount;
    }

    void onBytesUnbuffered(uint64_t amount) {
      bytesBuffered_ -= amount;
    }

    const MoQSettings* getMoqSettings() const {
      if (session_) {
        return &session_->getMoqSettings();
      }
      return nullptr;
    }

    MoQExecutor* getExecutor() const {
      return session_ ? session_->exec_.get() : nullptr;
    }

    MoQSessionBase* getSession() const {
      return session_;
    }

   protected:
    MoQSessionBase* session_{nullptr};
    FullTrackName fullTrackName_;
    RequestID requestID_;
    uint8_t subPriority_;
    GroupOrder groupOrder_;
    MoQFrameWriter moqFrameWriter_;
    uint64_t version_;
    uint64_t bytesBuffered_{0};
    uint64_t bytesBufferedThreshold_{0};
    std::optional<uint8_t> publisherPriority_;
  };

  // Track receive state forward declarations
  class TrackReceiveStateBase;
  class SubscribeTrackReceiveState;
  class FetchTrackReceiveState;
  friend class FetchTrackReceiveState;

  std::shared_ptr<SubscribeTrackReceiveState> getSubscribeTrackReceiveState(
      TrackAlias alias);
  std::shared_ptr<FetchTrackReceiveState> getFetchTrackReceiveState(
      RequestID requestID);

 protected:
  // Protected constructor - only derived classes can instantiate
  MoQSessionBase(
      MoQControlCodec::Direction dir,
      ExecutorPtr exec,
      ServerSetupCallback* serverSetupCallback = nullptr);

  // Core session state
  MoQControlCodec::Direction dir_;
  ExecutorPtr exec_;
  std::shared_ptr<MLogger> logger_ = nullptr;

  // Track management maps
  compat::FastMap<
      TrackAlias,
      std::shared_ptr<SubscribeTrackReceiveState>,
      TrackAlias::hash>
      subTracks_;
  compat::FastMap<
      RequestID,
      std::shared_ptr<FetchTrackReceiveState>,
      RequestID::hash>
      fetches_;
  compat::FastMap<RequestID, TrackAlias, RequestID::hash> reqIdToTrackAlias_;

  // Protected utility methods
  bool closeSessionIfRequestIDInvalid(
      RequestID requestID,
      bool skipCheck,
      bool isNewRequest,
      bool parityMatters = true);

  uint8_t getRequestIDMultiplier() const {
    return 2;
  }

  virtual void deliverBufferedData(TrackAlias trackAlias);
  void aliasifyAuthTokens(
      Parameters& params,
      const std::optional<uint64_t>& forceVersion = std::nullopt);
  virtual RequestID getNextRequestID();
  virtual void retireRequestID(bool signalWriteLoop);

  // Virtual cleanup method for proper inheritance pattern
  virtual void cleanup();

 public:
  // Send methods - public so track handles and publisher impls can call them
  virtual void publishNamespaceError(
      const PublishNamespaceError& publishNamespaceError);
  virtual void subscribeNamespaceError(
      const SubscribeNamespaceError& subscribeNamespaceError);
  virtual void trackStatusOk(const TrackStatusOk& trackStatusOk);
  virtual void trackStatusError(const TrackStatusError& trackStatusError);
  virtual void sendSubscribeOk(const SubscribeOk& subOk);
  virtual void subscribeError(const SubscribeError& subErr);
  virtual void unsubscribe(const Unsubscribe& unsubscribe);
  virtual void subscribeUpdate(const SubscribeUpdate& subUpdate);
  virtual void subscribeUpdateOk(const RequestOk& requestOk);
  virtual void subscribeUpdateError(
      const SubscribeUpdateError& requestError,
      RequestID subscriptionRequestID);
  virtual void sendPublishDone(const PublishDone& pubDone);
  virtual void fetchOk(const FetchOk& fetchOk);
  virtual void fetchError(const FetchError& fetchError);
  virtual void fetchCancel(const FetchCancel& fetchCancel);
  virtual void publishOk(const PublishOk& pubOk);
  virtual void publishError(const PublishError& publishError);

 protected:

  // Static helper methods
  static uint64_t getMaxRequestIDIfPresent(const SetupParameters& params);
  static uint64_t getMaxAuthTokenCacheSizeIfPresent(
      const SetupParameters& params);
  static std::optional<std::string> getMoQTImplementationIfPresent(
      const SetupParameters& params);
  static bool shouldIncludeMoqtImplementationParam(
      const std::vector<uint64_t>& supportedVersions);

  virtual void removeSubscriptionState(TrackAlias alias, RequestID id);
  virtual void checkForCloseOnDrain();
  virtual void sendMaxRequestID(bool signalWriteLoop);
  virtual void fetchComplete(RequestID requestID);
  virtual void initializeNegotiatedVersion(uint64_t negotiatedVersion);

  // Core frame writer and stats callbacks
  MoQFrameWriter moqFrameWriter_;
  std::shared_ptr<MoQPublisherStatsCallback> publisherStatsCallback_{nullptr};
  std::shared_ptr<MoQSubscriberStatsCallback> subscriberStatsCallback_{nullptr};

  // Handlers and cancellation
  compat::CancellationSource cancellationSource_;
  std::shared_ptr<Subscriber> subscribeHandler_;
  std::shared_ptr<Publisher> publishHandler_;

  // Private session state
  compat::FastMap<RequestID, std::shared_ptr<PublisherImpl>, RequestID::hash>
      pubTracks_;
  compat::FastMap<TrackAlias, std::list<Payload>, TrackAlias::hash>
      bufferedDatagrams_;

  uint64_t closedRequests_{0};
  uint64_t maxConcurrentRequests_{100};
  uint64_t peerMaxRequestID_{0};

  bool setupComplete_{false};
  bool draining_{false};
  bool receivedGoaway_{false};

  uint64_t nextRequestID_{0};
  uint64_t nextExpectedPeerRequestID_{0};
  uint64_t maxRequestID_{0};

  ServerSetupCallback* serverSetupCallback_{nullptr};
  MoQSessionCloseCallback* closeCallback_{nullptr};
  MoQSettings moqSettings_;

  std::optional<uint64_t> negotiatedVersion_;
  MoQControlCodec controlCodec_;
  MoQTokenCache tokenCache_{1024};
  bool closed_{false};
};

} // namespace moxygen
