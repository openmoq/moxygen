/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// Callback-based (compat) implementation of MoQSession
// This file is compiled when NOT using mvfst (std-mode or Folly+picoquic)

#include "moxygen/MoQSession.h"

#if !MOXYGEN_QUIC_MVFST

#include <algorithm>
#include <iostream>

namespace moxygen {

namespace {

// Helper: convert compat::Payload to Buffer for MoQFrameWriter methods
// In Folly mode, Buffer = folly::IOBuf
// In std mode, Buffer = ByteBuffer
inline std::unique_ptr<Buffer> payloadToBuffer(
    const std::unique_ptr<compat::Payload>& p) {
  if (!p) {
    return nullptr;
  }
#if MOXYGEN_USE_FOLLY
  // In Folly mode, extract the IOBuf from the wrapper and clone it
  auto* iobuf = p->getIOBuf();
  if (!iobuf) {
    return nullptr;
  }
  return iobuf->clone();
#else
  return Buffer::copyBuffer(p->data(), p->length());
#endif
}

#if MOXYGEN_USE_FOLLY
// Overload for moxygen::Payload (which is unique_ptr<IOBuf> in Folly mode)
// In std-mode, Payload = unique_ptr<compat::Payload>, same as above
inline std::unique_ptr<Buffer> payloadToBuffer(const Payload& p) {
  if (!p) {
    return nullptr;
  }
  return p->clone();
}
#endif

// Helper: convert ByteBuffer (unique_ptr<ByteBuffer>) to compat::Payload
// for writeStreamDataSync
inline std::unique_ptr<compat::Payload> bufferToPayload(
    std::unique_ptr<Buffer> buf) {
  return compat::Payload::wrap(std::move(buf));
}

// Helper: convert moxygen::Payload to compat::Payload
// In Folly mode: moxygen::Payload = unique_ptr<IOBuf>
// In std mode: moxygen::Payload = unique_ptr<compat::Payload>
inline std::unique_ptr<compat::Payload> toCompatPayload(Payload p) {
#if MOXYGEN_USE_FOLLY
  if (!p) {
    return nullptr;
  }
  return std::make_unique<compat::Payload>(std::move(p));
#else
  return std::move(p);
#endif
}

// =========================================================================
// Phase 8: Receive state classes
// =========================================================================

// Base class for track receive state (std-mode)
class TrackReceiveStateBase {
 public:
  TrackReceiveStateBase(FullTrackName fullTrackName, RequestID requestID)
      : fullTrackName_(std::move(fullTrackName)), requestID_(requestID) {}

  virtual ~TrackReceiveStateBase() = default;

  [[nodiscard]] const FullTrackName& fullTrackName() const {
    return fullTrackName_;
  }

  [[nodiscard]] RequestID getRequestID() const {
    return requestID_;
  }

  bool isCancelled() const {
    return cancelled_;
  }

 protected:
  FullTrackName fullTrackName_;
  RequestID requestID_;
  bool cancelled_{false};
};

} // anonymous namespace

// =========================================================================
// SubscribeTrackReceiveState (std-mode)
// =========================================================================

class MoQSessionBase::SubscribeTrackReceiveState {
 public:
  SubscribeTrackReceiveState(
      FullTrackName fullTrackName,
      RequestID requestID,
      std::shared_ptr<TrackConsumer> callback,
      MoQSessionBase* session,
      TrackAlias alias)
      : fullTrackName_(std::move(fullTrackName)),
        requestID_(requestID),
        callback_(std::move(callback)),
        session_(session),
        alias_(alias) {}

  ~SubscribeTrackReceiveState() = default;

  [[nodiscard]] const FullTrackName& fullTrackName() const {
    return fullTrackName_;
  }

  [[nodiscard]] RequestID getRequestID() const {
    return requestID_;
  }

  std::shared_ptr<TrackConsumer> getSubscribeCallback() const {
    return cancelled_ ? nullptr : callback_;
  }

  bool isCancelled() const {
    return cancelled_;
  }

  void cancel() {
    callback_.reset();
    cancelled_ = true;
    pendingPublishDone_.reset();
  }

  void onSubgroup() {
    streamCount_++;
    if (pendingPublishDone_ &&
        streamCount_ >= pendingPublishDone_->streamCount) {
      deliverPublishDoneAndRemove();
    }
  }

  void processPublishDone(PublishDone pubDone) {
    if (!callback_) {
      // Unsubscribe raced with publishDone - just remove state
      session_->removeSubscriptionState(alias_, requestID_);
      return;
    }
    if (pendingPublishDone_) {
      XLOG(ERR) << "Duplicate PUBLISH_DONE";
      session_->close(SessionCloseErrorCode::PROTOCOL_VIOLATION);
      return;
    }

    pendingPublishDone_ = std::move(pubDone);
    if (pendingPublishDone_->streamCount > streamCount_) {
      // Still waiting for streams - schedule timeout
      if (session_ && session_->getExecutor()) {
        auto* self = this;
        session_->getExecutor()->scheduleTimeout(
            [self]() {
              if (self->pendingPublishDone_) {
                self->deliverPublishDoneAndRemove();
              }
            },
            session_->getMoqSettings().publishDoneStreamCountTimeout);
      }
    } else {
      deliverPublishDoneAndRemove();
    }
  }

  void subscribeError(SubscribeError subErr) {
    if (callback_) {
      processPublishDone(
          {requestID_,
           PublishDoneStatusCode::SESSION_CLOSED,
           0,
           "closed locally"});
    }
  }

  void setCurrentStreamId(uint64_t id) {
    currentStreamId_ = id;
  }

  uint8_t getPublisherPriority() const {
    return publisherPriority_;
  }

  void setPublisherPriority(uint8_t priority) {
    publisherPriority_ = priority;
  }

  TrackAlias getAlias() const {
    return alias_;
  }

 private:
  void deliverPublishDoneAndRemove() {
    if (pendingPublishDone_ && callback_) {
      auto cb = std::exchange(callback_, nullptr);
      cb->publishDone(std::move(*pendingPublishDone_));
    }
    pendingPublishDone_.reset();
    session_->removeSubscriptionState(alias_, requestID_);
  }

  FullTrackName fullTrackName_;
  RequestID requestID_;
  std::shared_ptr<TrackConsumer> callback_;
  MoQSessionBase* session_{nullptr};
  TrackAlias alias_;
  bool cancelled_{false};
  uint64_t streamCount_{0};
  uint64_t currentStreamId_{0};
  std::optional<PublishDone> pendingPublishDone_;
  uint8_t publisherPriority_{kDefaultPriority};
};

// =========================================================================
// FetchTrackReceiveState (std-mode)
// =========================================================================

class MoQSessionBase::FetchTrackReceiveState {
 public:
  FetchTrackReceiveState(
      FullTrackName fullTrackName,
      RequestID requestID,
      std::shared_ptr<FetchConsumer> fetchCallback)
      : fullTrackName_(std::move(fullTrackName)),
        requestID_(requestID),
        callback_(std::move(fetchCallback)) {}

  [[nodiscard]] const FullTrackName& fullTrackName() const {
    return fullTrackName_;
  }

  [[nodiscard]] RequestID getRequestID() const {
    return requestID_;
  }

  std::shared_ptr<FetchConsumer> getFetchCallback() const {
    return callback_;
  }

  void resetFetchCallback(MoQSessionBase* session) {
    callback_.reset();
    if (fetchOkReceived_) {
      session->fetches_.erase(requestID_);
      session->checkForCloseOnDrain();
    }
  }

  void cancel(MoQSessionBase* session) {
    cancelled_ = true;
    callback_.reset();
    resetFetchCallback(session);
  }

  void fetchOK(FetchOk /*ok*/) {
    fetchOkReceived_ = true;
  }

  void onFetchHeader(RequestID /*requestID*/) {}

  void setCurrentStreamId(uint64_t id) {
    currentStreamId_ = id;
  }

  bool isCancelled() const {
    return cancelled_;
  }

 private:
  FullTrackName fullTrackName_;
  RequestID requestID_;
  std::shared_ptr<FetchConsumer> callback_;
  bool cancelled_{false};
  bool fetchOkReceived_{false};
  uint64_t currentStreamId_{0};
};

// Dummy TrackReceiveStateBase - just used to satisfy the forward decl
class MoQSessionBase::TrackReceiveStateBase {};

namespace {

// =========================================================================
// Phase 3/6: Publishing helpers
// =========================================================================

// CompatStreamPublisher: writes object data to a single uni stream
// Used for both subgroup and fetch data delivery
class CompatStreamPublisher : public SubgroupConsumer,
                              public FetchConsumer,
                              public std::enable_shared_from_this<CompatStreamPublisher> {
 public:
  CompatStreamPublisher(
      compat::StreamWriteHandle* writeHandle,
      MoQFrameWriter& frameWriter,
      std::shared_ptr<MoQSessionBase::PublisherImpl> publisher)
      : writeHandle_(writeHandle),
        publisher_(std::move(publisher)) {
    moqFrameWriter_.initializeVersion(publisher_->getVersion());
  }

  // --- SubgroupConsumer overrides ---

  compat::Expected<compat::Unit, MoQPublishError> object(
      uint64_t objectID,
      Payload payload,
      Extensions extensions,
      bool finSubgroup) override {
    std::cerr << "[DEBUG] Server sending object: id=" << objectID
              << " payloadLen=" << (payload ? payload->computeChainDataLength() : 0)
              << " fin=" << finSubgroup << "\n";
    if (!writeHandle_ || writeHandle_->isCancelled()) {
      return compat::makeUnexpected(
          MoQPublishError(MoQPublishError::CANCELLED, "stream cancelled"));
    }
    ObjectHeader header;
    header.id = objectID;
    header.length = payload ? payload->computeChainDataLength() : 0;
    header.status = ObjectStatus::NORMAL;
    header.extensions = std::move(extensions);

    compat::ByteBufferQueue writeBuf;
    moqFrameWriter_.writeStreamObject(
        writeBuf, StreamType::SUBGROUP_HEADER_SG, header, payloadToBuffer(payload));
    auto data = writeBuf.move();
    if (data) {
      writeHandle_->writeStreamDataSync(bufferToPayload(std::move(data)), finSubgroup);
    }
    if (finSubgroup) {
      writeHandle_ = nullptr;
    }
    return compat::unit;
  }

  compat::Expected<compat::Unit, MoQPublishError> beginObject(
      uint64_t objectID,
      uint64_t length,
      Payload initialPayload,
      Extensions extensions) override {
    if (!writeHandle_ || writeHandle_->isCancelled()) {
      return compat::makeUnexpected(
          MoQPublishError(MoQPublishError::CANCELLED, "stream cancelled"));
    }
    ObjectHeader header;
    header.id = objectID;
    header.length = length;
    header.status = ObjectStatus::NORMAL;
    header.extensions = std::move(extensions);

    compat::ByteBufferQueue writeBuf;
    moqFrameWriter_.writeStreamObject(
        writeBuf, StreamType::SUBGROUP_HEADER_SG, header,
        payloadToBuffer(initialPayload));
    auto data = writeBuf.move();
    if (data) {
      writeHandle_->writeStreamDataSync(bufferToPayload(std::move(data)), false);
    }
    return compat::unit;
  }

  compat::Expected<ObjectPublishStatus, MoQPublishError> objectPayload(
      Payload payload,
      bool finSubgroup) override {
    if (!writeHandle_ || writeHandle_->isCancelled()) {
      return compat::makeUnexpected(
          MoQPublishError(MoQPublishError::CANCELLED, "stream cancelled"));
    }
    if (payload) {
      writeHandle_->writeStreamDataSync(
          toCompatPayload(std::move(payload)), finSubgroup);
    }
    if (finSubgroup) {
      writeHandle_ = nullptr;
    }
    return ObjectPublishStatus::DONE;
  }

  compat::Expected<compat::Unit, MoQPublishError> endOfGroup(
      uint64_t endOfGroupObjectID) override {
    if (!writeHandle_) {
      return compat::makeUnexpected(
          MoQPublishError(MoQPublishError::CANCELLED, "stream closed"));
    }
    ObjectHeader header;
    header.id = endOfGroupObjectID;
    header.status = ObjectStatus::END_OF_GROUP;
    header.length = 0;

    compat::ByteBufferQueue writeBuf;
    moqFrameWriter_.writeStreamObject(
        writeBuf, StreamType::SUBGROUP_HEADER_SG, header, nullptr);
    auto data = writeBuf.move();
    if (data) {
      writeHandle_->writeStreamDataSync(bufferToPayload(std::move(data)), true);
    }
    writeHandle_ = nullptr;
    return compat::unit;
  }

  compat::Expected<compat::Unit, MoQPublishError> endOfTrackAndGroup(
      uint64_t endOfTrackObjectID) override {
    if (!writeHandle_) {
      return compat::makeUnexpected(
          MoQPublishError(MoQPublishError::CANCELLED, "stream closed"));
    }
    ObjectHeader header;
    header.id = endOfTrackObjectID;
    header.status = ObjectStatus::END_OF_TRACK;
    header.length = 0;

    compat::ByteBufferQueue writeBuf;
    moqFrameWriter_.writeStreamObject(
        writeBuf, StreamType::SUBGROUP_HEADER_SG, header, nullptr);
    auto data = writeBuf.move();
    if (data) {
      writeHandle_->writeStreamDataSync(bufferToPayload(std::move(data)), true);
    }
    writeHandle_ = nullptr;
    return compat::unit;
  }

  compat::Expected<compat::Unit, MoQPublishError> endOfSubgroup() override {
    if (writeHandle_) {
      writeHandle_->writeStreamDataSync(nullptr, true);
      writeHandle_ = nullptr;
    }
    return compat::unit;
  }

  void reset(ResetStreamErrorCode error) override {
    if (writeHandle_) {
      writeHandle_->resetStream(static_cast<uint32_t>(error));
      writeHandle_ = nullptr;
    }
  }

  // --- FetchConsumer overrides ---

  compat::Expected<compat::Unit, MoQPublishError> object(
      uint64_t groupID,
      uint64_t subgroupID,
      uint64_t objectID,
      Payload payload,
      Extensions extensions,
      bool finFetch) override {
    if (!writeHandle_ || writeHandle_->isCancelled()) {
      return compat::makeUnexpected(
          MoQPublishError(MoQPublishError::CANCELLED, "stream cancelled"));
    }
    ObjectHeader header;
    header.group = groupID;
    header.subgroup = subgroupID;
    header.id = objectID;
    header.length = payload ? payload->computeChainDataLength() : 0;
    header.status = ObjectStatus::NORMAL;
    header.extensions = std::move(extensions);

    compat::ByteBufferQueue writeBuf;
    moqFrameWriter_.writeStreamObject(
        writeBuf, StreamType::FETCH_HEADER, header,
        payloadToBuffer(payload));
    auto data = writeBuf.move();
    if (data) {
      writeHandle_->writeStreamDataSync(bufferToPayload(std::move(data)), finFetch);
    }
    if (finFetch) {
      writeHandle_ = nullptr;
    }
    return compat::unit;
  }

  compat::Expected<compat::Unit, MoQPublishError> beginObject(
      uint64_t groupID,
      uint64_t subgroupID,
      uint64_t objectID,
      uint64_t length,
      Payload initialPayload,
      Extensions extensions) override {
    if (!writeHandle_ || writeHandle_->isCancelled()) {
      return compat::makeUnexpected(
          MoQPublishError(MoQPublishError::CANCELLED, "stream cancelled"));
    }
    ObjectHeader header;
    header.group = groupID;
    header.subgroup = subgroupID;
    header.id = objectID;
    header.length = length;
    header.status = ObjectStatus::NORMAL;
    header.extensions = std::move(extensions);

    compat::ByteBufferQueue writeBuf;
    moqFrameWriter_.writeStreamObject(
        writeBuf, StreamType::FETCH_HEADER, header,
        payloadToBuffer(initialPayload));
    auto data = writeBuf.move();
    if (data) {
      writeHandle_->writeStreamDataSync(bufferToPayload(std::move(data)), false);
    }
    return compat::unit;
  }

  compat::Expected<compat::Unit, MoQPublishError> endOfGroup(
      uint64_t groupID,
      uint64_t subgroupID,
      uint64_t objectID,
      bool finFetch) override {
    if (!writeHandle_) {
      return compat::makeUnexpected(
          MoQPublishError(MoQPublishError::CANCELLED, "stream closed"));
    }
    ObjectHeader header;
    header.group = groupID;
    header.subgroup = subgroupID;
    header.id = objectID;
    header.status = ObjectStatus::END_OF_GROUP;
    header.length = 0;

    compat::ByteBufferQueue writeBuf;
    moqFrameWriter_.writeStreamObject(
        writeBuf, StreamType::FETCH_HEADER, header, nullptr);
    auto data = writeBuf.move();
    if (data) {
      writeHandle_->writeStreamDataSync(bufferToPayload(std::move(data)), finFetch);
    }
    if (finFetch) {
      writeHandle_ = nullptr;
    }
    return compat::unit;
  }

  compat::Expected<compat::Unit, MoQPublishError> endOfTrackAndGroup(
      uint64_t groupID,
      uint64_t subgroupID,
      uint64_t objectID) override {
    if (!writeHandle_) {
      return compat::makeUnexpected(
          MoQPublishError(MoQPublishError::CANCELLED, "stream closed"));
    }
    ObjectHeader header;
    header.group = groupID;
    header.subgroup = subgroupID;
    header.id = objectID;
    header.status = ObjectStatus::END_OF_TRACK;
    header.length = 0;

    compat::ByteBufferQueue writeBuf;
    moqFrameWriter_.writeStreamObject(
        writeBuf, StreamType::FETCH_HEADER, header, nullptr);
    auto data = writeBuf.move();
    if (data) {
      writeHandle_->writeStreamDataSync(bufferToPayload(std::move(data)), true);
    }
    writeHandle_ = nullptr;
    return compat::unit;
  }

  compat::Expected<compat::Unit, MoQPublishError> endOfFetch() override {
    if (writeHandle_) {
      writeHandle_->writeStreamDataSync(nullptr, true);
      writeHandle_ = nullptr;
    }
    if (publisher_) {
      publisher_->fetchComplete();
    }
    return compat::unit;
  }

  compat::Expected<compat::SemiFuture<uint64_t>, MoQPublishError>
  awaitReadyToConsume() override {
    return compat::makeSemiFuture<uint64_t>(0);
  }

 private:
  compat::StreamWriteHandle* writeHandle_{nullptr};
  std::shared_ptr<MoQSessionBase::PublisherImpl> publisher_;
  MoQFrameWriter moqFrameWriter_;
};

// =========================================================================
// CompatTrackPublisher: PublisherImpl + TrackConsumer for server-side subscribe
// =========================================================================

class CompatTrackPublisher : public MoQSessionBase::PublisherImpl,
                             public TrackConsumer {
 public:
  CompatTrackPublisher(
      MoQSessionBase* session,
      compat::WebTransportInterface* wt,
      FullTrackName fullTrackName,
      RequestID requestID,
      Priority subPriority,
      GroupOrder groupOrder,
      uint64_t version)
      : PublisherImpl(
            session,
            std::move(fullTrackName),
            requestID,
            subPriority,
            groupOrder,
            version,
            0),
        wt_(wt) {}

  // TrackConsumer interface
  compat::Expected<compat::Unit, MoQPublishError> setTrackAlias(
      TrackAlias alias) override {
    if (trackAlias_ && alias != *trackAlias_) {
      return compat::makeUnexpected(MoQPublishError(
          MoQPublishError::API_ERROR,
          "Track Alias already set to different value"));
    }
    trackAlias_ = alias;
    return compat::unit;
  }

  compat::Expected<std::shared_ptr<SubgroupConsumer>, MoQPublishError>
  beginSubgroup(
      uint64_t groupID,
      uint64_t subgroupID,
      Priority priority,
      bool /*containsLastInGroup*/ = false) override {
    if (!wt_ || !trackAlias_) {
      return compat::makeUnexpected(
          MoQPublishError(MoQPublishError::CANCELLED, "session closed"));
    }

    auto streamRes = wt_->createUniStream();
    if (!streamRes) {
      return compat::makeUnexpected(
          MoQPublishError(MoQPublishError::BLOCKED, "stream creation failed"));
    }
    auto* writeHandle = streamRes.value();

    // Write subgroup header
    ObjectHeader header;
    header.group = groupID;
    header.subgroup = subgroupID;
    header.priority = priority;

    compat::ByteBufferQueue writeBuf;
    moqFrameWriter_.writeSubgroupHeader(
        writeBuf, *trackAlias_, header);
    auto data = writeBuf.move();
    if (data) {
      writeHandle->writeStreamDataSync(bufferToPayload(std::move(data)), false);
    }

    onStreamCreated();
    auto pub = std::make_shared<CompatStreamPublisher>(
        writeHandle, moqFrameWriter_, shared_from_this());
    return pub;
  }

  compat::Expected<compat::SemiFuture<compat::Unit>, MoQPublishError>
  awaitStreamCredit() override {
    return compat::SemiFuture<compat::Unit>(compat::unit);
  }

  compat::Expected<compat::Unit, MoQPublishError> objectStream(
      const ObjectHeader& header,
      Payload payload,
      bool /*lastInGroup*/ = false) override {
    if (!wt_ || !trackAlias_) {
      return compat::makeUnexpected(
          MoQPublishError(MoQPublishError::CANCELLED, "session closed"));
    }

    auto streamRes = wt_->createUniStream();
    if (!streamRes) {
      return compat::makeUnexpected(
          MoQPublishError(MoQPublishError::BLOCKED, "stream creation failed"));
    }
    auto* writeHandle = streamRes.value();

    compat::ByteBufferQueue writeBuf;
    moqFrameWriter_.writeSingleObjectStream(
        writeBuf, *trackAlias_, header,
        payloadToBuffer(payload));
    auto data = writeBuf.move();
    if (data) {
      writeHandle->writeStreamDataSync(bufferToPayload(std::move(data)), true);
    }

    onStreamCreated();
    return compat::unit;
  }

  compat::Expected<compat::Unit, MoQPublishError> datagram(
      const ObjectHeader& header,
      Payload payload,
      bool /*lastInGroup*/ = false) override {
    if (!wt_ || !trackAlias_) {
      return compat::makeUnexpected(
          MoQPublishError(MoQPublishError::CANCELLED, "session closed"));
    }

    compat::ByteBufferQueue writeBuf;
    moqFrameWriter_.writeDatagramObject(
        writeBuf, *trackAlias_, header,
        payloadToBuffer(payload));
    auto data = writeBuf.move();
    if (data) {
      wt_->sendDatagram(bufferToPayload(std::move(data)));
    }
    return compat::unit;
  }

  compat::Expected<compat::Unit, MoQPublishError> publishDone(
      PublishDone pubDone) override {
    pubDone.requestID = requestID_;
    pubDone.streamCount = streamCount_;
    if (session_) {
      subscriptionHandle_.reset();
      session_->sendPublishDone(pubDone);
    }
    return compat::unit;
  }

  void setSubscriptionHandle(
      std::shared_ptr<Publisher::SubscriptionHandle> handle) {
    if (!trackAlias_) {
      trackAlias_ = handle->subscribeOk().trackAlias;
    }
    subscriptionHandle_ = std::move(handle);
  }

  void subscribeOkSent(const SubscribeOk& subOk) {
    if (!trackAlias_) {
      trackAlias_ = subOk.trackAlias;
    }
  }

  // PublisherImpl virtual overrides
  void terminatePublish(
      PublishDone pubDone,
      ResetStreamErrorCode /*error*/) override {
    // Reset all active subgroups, then send PublishDone
    subscriptionHandle_.reset();
    if (session_) {
      session_->sendPublishDone(pubDone);
    }
  }

  void onStreamCreated() override {
    streamCount_++;
  }

  void onStreamComplete(const ObjectHeader& /*finalHeader*/) override {}

  void onTooManyBytesBuffered() override {}

  bool isCancelled() const {
    return !session_;
  }

 private:
  compat::WebTransportInterface* wt_{nullptr};
  std::optional<TrackAlias> trackAlias_;
  std::shared_ptr<Publisher::SubscriptionHandle> subscriptionHandle_;
  uint64_t streamCount_{0};
};

// =========================================================================
// CompatFetchPublisher: PublisherImpl for server-side fetch handling
// =========================================================================

class CompatFetchPublisher : public MoQSessionBase::PublisherImpl {
 public:
  CompatFetchPublisher(
      MoQSessionBase* session,
      compat::WebTransportInterface* wt,
      FullTrackName fullTrackName,
      RequestID requestID,
      Priority subPriority,
      GroupOrder groupOrder,
      uint64_t version)
      : PublisherImpl(
            session,
            std::move(fullTrackName),
            requestID,
            subPriority,
            groupOrder,
            version,
            0),
        wt_(wt) {
    initialize();
  }

  void initialize() {
    if (!wt_) {
      return;
    }
    auto streamRes = wt_->createUniStream();
    if (!streamRes) {
      return;
    }
    writeHandle_ = streamRes.value();

    // Write fetch header
    compat::ByteBufferQueue writeBuf;
    moqFrameWriter_.writeFetchHeader(writeBuf, requestID_);
    auto data = writeBuf.move();
    if (data) {
      writeHandle_->writeStreamDataSync(bufferToPayload(std::move(data)), false);
    }
  }

  std::shared_ptr<FetchConsumer> getStreamPublisher() {
    if (!writeHandle_) {
      return nullptr;
    }
    if (!streamPublisher_) {
      streamPublisher_ = std::make_shared<CompatStreamPublisher>(
          writeHandle_, moqFrameWriter_, shared_from_this());
    }
    return streamPublisher_;
  }

  void reset(ResetStreamErrorCode error) {
    if (streamPublisher_) {
      streamPublisher_->reset(error);
      streamPublisher_.reset();
    }
  }

  // PublisherImpl virtual overrides
  void terminatePublish(
      PublishDone /*pubDone*/,
      ResetStreamErrorCode error) override {
    reset(error);
  }

  void onStreamComplete(const ObjectHeader& /*finalHeader*/) override {}

  void onTooManyBytesBuffered() override {}

  bool isCancelled() const {
    return !session_;
  }

 private:
  compat::WebTransportInterface* wt_{nullptr};
  compat::StreamWriteHandle* writeHandle_{nullptr};
  std::shared_ptr<CompatStreamPublisher> streamPublisher_;
};

// =========================================================================
// Phase 5: CompatObjectStreamCallback for uni stream data delivery
// =========================================================================

class CompatObjectStreamCallback
    : public MoQObjectStreamCodec::ObjectCallback {
 public:
  using GetSubscribeStateFn = std::function<
      std::shared_ptr<MoQSessionBase::SubscribeTrackReceiveState>(
          TrackAlias alias)>;

  using GetFetchStateFn = std::function<
      std::shared_ptr<MoQSessionBase::FetchTrackReceiveState>(
          RequestID requestID)>;

  CompatObjectStreamCallback(
      MoQSessionBase* session,
      GetSubscribeStateFn getSubState,
      GetFetchStateFn getFetchState)
      : session_(session),
        getSubStateFn_(std::move(getSubState)),
        getFetchStateFn_(std::move(getFetchState)) {}

  void setCurrentStreamId(uint64_t id) {
    currentStreamId_ = id;
  }

  MoQCodec::ParseResult onSubgroup(
      TrackAlias alias,
      uint64_t group,
      uint64_t subgroup,
      std::optional<uint8_t> priority,
      const SubgroupOptions& /*options*/) override {
    trackAlias_ = alias;
    auto subscribeState = getSubStateFn_(alias);
    if (!subscribeState) {
      // State not ready (alias not yet mapped from SUBSCRIBE_OK)
      // Store for deferred resolution
      deferredAlias_ = alias;
      deferredGroup_ = group;
      deferredSubgroup_ = subgroup;
      deferredPriority_ = priority;
      return MoQCodec::ParseResult::BLOCKED;
    }

    subscribeState_ = std::move(subscribeState);
    session_->onSubscriptionStreamOpenedByPeer();
    auto callback = subscribeState_->getSubscribeCallback();
    if (!callback) {
      return MoQCodec::ParseResult::ERROR_TERMINATE;
    }

    uint8_t effectivePriority =
        priority.value_or(subscribeState_->getPublisherPriority());
    auto res = callback->beginSubgroup(group, subgroup, effectivePriority);
    if (res.hasValue()) {
      subgroupCallback_ = *res;
    } else {
      return MoQCodec::ParseResult::ERROR_TERMINATE;
    }

    subscribeState_->setCurrentStreamId(currentStreamId_);
    subscribeState_->onSubgroup();
    return MoQCodec::ParseResult::CONTINUE;
  }

  MoQCodec::ParseResult retrySubgroup() {
    if (!deferredAlias_) {
      return MoQCodec::ParseResult::ERROR_TERMINATE;
    }
    auto alias = *deferredAlias_;
    auto group = deferredGroup_;
    auto subgroup = deferredSubgroup_;
    auto priority = deferredPriority_;
    deferredAlias_.reset();
    return onSubgroup(alias, group, subgroup, priority, SubgroupOptions());
  }

  MoQCodec::ParseResult onFetchHeader(RequestID requestID) override {
    fetchState_ = getFetchStateFn_(requestID);
    if (!fetchState_) {
      return MoQCodec::ParseResult::ERROR_TERMINATE;
    }
    fetchState_->setCurrentStreamId(currentStreamId_);
    fetchState_->onFetchHeader(requestID);
    return MoQCodec::ParseResult::CONTINUE;
  }

  MoQCodec::ParseResult onObjectBegin(
      uint64_t group,
      uint64_t subgroup,
      uint64_t objectID,
      Extensions extensions,
      uint64_t length,
      Payload initialPayload,
      bool objectComplete,
      bool streamComplete) override {
    if (isCancelled()) {
      return MoQCodec::ParseResult::ERROR_TERMINATE;
    }

    compat::Expected<compat::Unit, MoQPublishError> res{compat::unit};
    if (objectComplete) {
      if (subgroupCallback_) {
        res = subgroupCallback_->object(
            objectID, std::move(initialPayload),
            std::move(extensions), streamComplete);
        if (streamComplete) {
          endOfSubgroup();
        }
      } else if (fetchState_ && fetchState_->getFetchCallback()) {
        res = fetchState_->getFetchCallback()->object(
            group, subgroup, objectID,
            std::move(initialPayload), std::move(extensions), streamComplete);
        if (streamComplete) {
          endOfFetch(true);
        }
      }
    } else {
      if (subgroupCallback_) {
        res = subgroupCallback_->beginObject(
            objectID, length, std::move(initialPayload),
            std::move(extensions));
      } else if (fetchState_ && fetchState_->getFetchCallback()) {
        res = fetchState_->getFetchCallback()->beginObject(
            group, subgroup, objectID, length,
            std::move(initialPayload), std::move(extensions));
      }
    }
    return res ? MoQCodec::ParseResult::CONTINUE
               : MoQCodec::ParseResult::ERROR_TERMINATE;
  }

  MoQCodec::ParseResult onObjectPayload(Payload payload, bool objectComplete)
      override {
    if (isCancelled()) {
      return MoQCodec::ParseResult::ERROR_TERMINATE;
    }
    bool finStream = false;
    if (subgroupCallback_) {
      auto res = subgroupCallback_->objectPayload(std::move(payload), finStream);
      if (!res) {
        return MoQCodec::ParseResult::ERROR_TERMINATE;
      }
    } else if (fetchState_ && fetchState_->getFetchCallback()) {
      auto res = fetchState_->getFetchCallback()->objectPayload(
          std::move(payload), finStream);
      if (!res) {
        return MoQCodec::ParseResult::ERROR_TERMINATE;
      }
    }
    return MoQCodec::ParseResult::CONTINUE;
  }

  MoQCodec::ParseResult onObjectStatus(
      uint64_t group,
      uint64_t subgroup,
      uint64_t objectID,
      std::optional<uint8_t> /*priority*/,
      ObjectStatus status) override {
    if (isCancelled()) {
      return MoQCodec::ParseResult::ERROR_TERMINATE;
    }
    compat::Expected<compat::Unit, MoQPublishError> res{compat::unit};
    switch (status) {
      case ObjectStatus::NORMAL:
      case ObjectStatus::OBJECT_NOT_EXIST:
      case ObjectStatus::GROUP_NOT_EXIST:
        if (!fetchState_ && status == ObjectStatus::GROUP_NOT_EXIST) {
          endOfSubgroup();
        }
        break;
      case ObjectStatus::END_OF_GROUP:
        if (fetchState_ && fetchState_->getFetchCallback()) {
          res = fetchState_->getFetchCallback()->endOfGroup(
              group, subgroup, objectID, false);
        } else if (subgroupCallback_) {
          res = subgroupCallback_->endOfGroup(objectID);
          endOfSubgroup();
        }
        break;
      case ObjectStatus::END_OF_TRACK:
        if (fetchState_ && fetchState_->getFetchCallback()) {
          res = fetchState_->getFetchCallback()->endOfTrackAndGroup(
              group, subgroup, objectID);
        } else if (subgroupCallback_) {
          res = subgroupCallback_->endOfTrackAndGroup(objectID);
        }
        endOfSubgroup();
        break;
    }
    return res ? MoQCodec::ParseResult::CONTINUE
               : MoQCodec::ParseResult::ERROR_TERMINATE;
  }

  void onEndOfStream() override {
    if (!isCancelled()) {
      if (fetchState_ && fetchState_->getFetchCallback()) {
        fetchState_->getFetchCallback()->endOfFetch();
        endOfFetch(true);
      } else if (subgroupCallback_) {
        subgroupCallback_->endOfSubgroup();
        endOfSubgroup();
      }
    }
  }

  void onConnectionError(ErrorCode error) override {
    session_->close(error);
  }

  bool reset(ResetStreamErrorCode error) {
    if (!subscribeState_ && !fetchState_) {
      return false;
    }
    if (!isCancelled()) {
      if (subgroupCallback_) {
        subgroupCallback_->reset(error);
      } else if (fetchState_ && fetchState_->getFetchCallback()) {
        fetchState_->getFetchCallback()->reset(error);
      }
    }
    endOfSubgroup();
    return true;
  }

  bool isDeferred() const {
    return deferredAlias_.has_value();
  }

  std::optional<TrackAlias> getDeferredAlias() const {
    return deferredAlias_;
  }

 private:
  bool isCancelled() const {
    if (fetchState_) {
      return !fetchState_->getFetchCallback();
    } else if (subscribeState_) {
      return !subgroupCallback_ || subscribeState_->isCancelled();
    }
    return true;
  }

  void endOfSubgroup() {
    if (subgroupCallback_) {
      session_->onSubscriptionStreamClosedByPeer();
    }
    subgroupCallback_.reset();
  }

  void endOfFetch(bool deliverCallback) {
    if (deliverCallback && fetchState_) {
      fetchState_->resetFetchCallback(session_);
    }
    fetchState_.reset();
  }

  MoQSessionBase* session_{nullptr};
  GetSubscribeStateFn getSubStateFn_;
  GetFetchStateFn getFetchStateFn_;
  std::shared_ptr<MoQSessionBase::SubscribeTrackReceiveState> subscribeState_;
  std::shared_ptr<SubgroupConsumer> subgroupCallback_;
  std::shared_ptr<MoQSessionBase::FetchTrackReceiveState> fetchState_;
  uint64_t currentStreamId_{0};
  TrackAlias trackAlias_{0};

  // Deferred subgroup state (when alias not yet known)
  std::optional<TrackAlias> deferredAlias_;
  uint64_t deferredGroup_{0};
  uint64_t deferredSubgroup_{0};
  std::optional<uint8_t> deferredPriority_;
};

// =========================================================================
// CompatReceiverSubscriptionHandle: returned to client on SUBSCRIBE_OK
// =========================================================================

class CompatReceiverSubscriptionHandle : public SubscriptionHandle {
 public:
  CompatReceiverSubscriptionHandle(
      SubscribeOk ok,
      TrackAlias alias,
      std::shared_ptr<MoQSession> session)
      : SubscriptionHandle(std::move(ok)),
        trackAlias_(alias),
        session_(std::move(session)) {}

  void unsubscribe() override {
    if (session_) {
      session_->unsubscribe({subscribeOk_->requestID});
      session_.reset();
    }
  }

  void subscribeUpdateWithCallback(
      SubscribeUpdate subUpdate,
      std::shared_ptr<
          compat::ResultCallback<SubscribeUpdateOk, SubscribeUpdateError>>
          callback) override {
    if (!session_) {
      callback->onError(SubscribeUpdateError{
          subUpdate.requestID,
          RequestErrorCode::INTERNAL_ERROR,
          "Session closed"});
      return;
    }
    subUpdate.existingRequestID = subscribeOk_->requestID;
    session_->subscribeUpdate(subUpdate);
    // Fire-and-forget for now (no response tracking)
    callback->onSuccess(SubscribeUpdateOk{.requestID = subUpdate.requestID});
  }

 private:
  TrackAlias trackAlias_;
  std::shared_ptr<MoQSession> session_;
};

// =========================================================================
// CompatReceiverFetchHandle: returned to client on FETCH_OK
// =========================================================================

class CompatReceiverFetchHandle : public Publisher::FetchHandle {
 public:
  CompatReceiverFetchHandle(FetchOk ok, std::shared_ptr<MoQSession> session)
      : FetchHandle(std::move(ok)), session_(std::move(session)) {}

  void fetchCancel() override {
    if (session_) {
      session_->fetchCancel({fetchOk_->requestID});
      session_.reset();
    }
  }

 private:
  std::shared_ptr<MoQSession> session_;
};

// State for a uni stream with its codec and callback
struct UniStreamState {
  std::unique_ptr<MoQObjectStreamCodec> codec;
  std::shared_ptr<CompatObjectStreamCallback> callback;
};

} // anonymous namespace

// =========================================================================
// MoQSessionBase implementation
// =========================================================================

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

void MoQSessionBase::goaway(Goaway /*goway*/) {}

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

void MoQSessionBase::sendMaxRequestID(bool /*signalWriteLoop*/) {}

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

// Response helpers - base implementations (no-op, overridden by MoQSession)
void MoQSessionBase::sendSubscribeOk(const SubscribeOk& /*subOk*/) {}
void MoQSessionBase::subscribeError(const SubscribeError& /*subErr*/) {}
void MoQSessionBase::unsubscribe(const Unsubscribe& /*unsub*/) {}
void MoQSessionBase::subscribeUpdate(const SubscribeUpdate& /*subUpdate*/) {}
void MoQSessionBase::subscribeUpdateOk(const RequestOk& /*requestOk*/) {}
void MoQSessionBase::subscribeUpdateError(
    const SubscribeUpdateError& /*requestError*/,
    RequestID /*subscriptionRequestID*/) {}
void MoQSessionBase::sendPublishDone(const PublishDone& /*pubDone*/) {}
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


// =========================================================================
// MoQSession (std-mode) implementation
// =========================================================================

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
  controlWriteHandle_ = nullptr;
  controlReadHandle_ = nullptr;
  controlStreamReady_ = false;
  pendingSubscribes_.clear();
  pendingFetches_.clear();
  MoQSessionBase::cleanup();
}

// =========================================================================
// Phase 1: Control Stream Plumbing
// =========================================================================

void MoQSession::flushControlBuf() {
  if (!controlWriteHandle_ || controlWriteBuf_.empty()) {
    return;
  }
  auto data = controlWriteBuf_.move();
  if (data) {
    controlWriteHandle_->writeStreamDataSync(bufferToPayload(std::move(data)), false);
  }
}

void MoQSession::setupControlReadCallback(compat::StreamReadHandle* readHandle) {
  auto self = shared_from_this();
  readHandle->setReadCallback(
      [self](compat::StreamData streamData, std::optional<uint32_t> error) {
        if (error) {
          XLOG(ERR) << "Control stream read error: " << *error
                    << " sess=" << self.get();
          self->close(SessionCloseErrorCode::INTERNAL_ERROR);
          return;
        }
        if (streamData.data) {
          auto result = self->controlCodec_.onIngress(
              payloadToBuffer(streamData.data), streamData.fin);
          if (result == MoQCodec::ParseResult::ERROR_TERMINATE) {
            XLOG(ERR) << "Control codec parse error sess=" << self.get();
            self->close(SessionCloseErrorCode::INTERNAL_ERROR);
          }
        }
        if (streamData.fin) {
          XLOG(INFO) << "Control stream FIN received sess=" << self.get();
          self->close(SessionCloseErrorCode::NO_ERROR);
        }
      });
}

void MoQSession::start() {
  if (!wt_) {
    XLOG(ERR) << "Cannot start session without transport";
    return;
  }

  if (dir_ == MoQControlCodec::Direction::CLIENT) {
    // Client: create the bidi control stream
    auto cs = wt_->createBidiStream();
    if (!cs) {
      XLOG(ERR) << "Failed to create control stream sess=" << this;
      close(SessionCloseErrorCode::INTERNAL_ERROR);
      return;
    }
    auto* bidiHandle = cs.value();
    controlWriteHandle_ = bidiHandle->writeHandle();
    controlReadHandle_ = bidiHandle->readHandle();
    controlStreamReady_ = true;

    // Set up read callback for incoming control messages
    setupControlReadCallback(controlReadHandle_);

    // Flush any buffered data (CLIENT_SETUP from setupWithCallback)
    flushControlBuf();
  }
  // Server side: control stream is established via onNewBidiStream
}

void MoQSession::drain() {
  draining_ = true;
  checkForCloseOnDrain();
}

void MoQSession::close(SessionCloseErrorCode error) {
  if (closed_) {
    return;
  }
  closed_ = true;

  // Fail pending subscribes
  for (auto& [reqId, pending] : pendingSubscribes_) {
    if (pending.callback) {
      pending.callback->onError(SubscribeError{
          reqId, SubscribeErrorCode::INTERNAL_ERROR, "Session closed"});
    }
  }
  pendingSubscribes_.clear();

  // Fail pending fetches
  for (auto& [reqId, pending] : pendingFetches_) {
    if (pending.callback) {
      pending.callback->onError(FetchError{
          reqId, FetchErrorCode::INTERNAL_ERROR, "Session closed"});
    }
  }
  pendingFetches_.clear();

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
    return;
  }
  // Flush if control stream is already ready (start() was called before us)
  if (controlStreamReady_) {
    flushControlBuf();
  }
}

Subscriber::PublishResult MoQSession::publish(
    PublishRequest pub,
    std::shared_ptr<Publisher::SubscriptionHandle> /*handle*/) {
  return compat::makeUnexpected(PublishError{
      pub.requestID, PublishErrorCode::NOT_SUPPORTED, "Not implemented"});
}

// =========================================================================
// Phase 1c: WebTransport callbacks
// =========================================================================

void MoQSession::onNewBidiStream(compat::BidiStreamHandle* bh) {
  if (!bh) {
    return;
  }
  if (dir_ == MoQControlCodec::Direction::SERVER && !controlStreamReady_) {
    // First bidi stream from client is the control stream
    controlWriteHandle_ = bh->writeHandle();
    controlReadHandle_ = bh->readHandle();
    controlStreamReady_ = true;

    setupControlReadCallback(controlReadHandle_);
    // SERVER_SETUP will be flushed after onClientSetup writes it
  } else {
    XLOG(WARN) << "Unexpected bidi stream sess=" << this;
  }
}

void MoQSession::onNewUniStream(compat::StreamReadHandle* rh) {
  if (!rh || !negotiatedVersion_) {
    return;
  }

  auto self = shared_from_this();
  auto streamId = rh->getID();

  // Create codec and callback, stored in a shared_ptr so the lambda captures it
  auto getSubState = [self](TrackAlias alias) {
    return self->getSubscribeTrackReceiveState(alias);
  };
  auto getFetchState = [self](RequestID requestID) {
    return self->getFetchTrackReceiveState(requestID);
  };

  auto objCallback = std::make_shared<CompatObjectStreamCallback>(
      this, std::move(getSubState), std::move(getFetchState));
  objCallback->setCurrentStreamId(streamId);

  auto codec = std::make_shared<MoQObjectStreamCodec>(objCallback.get());
  codec->initializeVersion(*negotiatedVersion_);
  codec->setStreamId(streamId);

  rh->setReadCallback(
      [self, codec, objCallback, streamId](
          compat::StreamData streamData, std::optional<uint32_t> error) {
        if (error) {
          XLOG(ERR) << "Uni stream read error: " << *error
                    << " id=" << streamId << " sess=" << self.get();
          ResetStreamErrorCode errorCode{ResetStreamErrorCode::INTERNAL_ERROR};
          objCallback->reset(errorCode);
          return;
        }
        if (streamData.data || streamData.fin) {
          auto result = codec->onIngress(
              payloadToBuffer(streamData.data), streamData.fin);

          if (result == MoQCodec::ParseResult::BLOCKED) {
            // Alias not yet known - store deferred state
            // The deferred data will be resolved when onSubscribeOk arrives
            // The codec has already buffered the excess data internally
            XLOG(DBG4) << "Uni stream BLOCKED (alias unknown) id=" << streamId;
            // We'll retry when deliverBufferedData is called
          } else if (result == MoQCodec::ParseResult::ERROR_TERMINATE) {
            if (!streamData.fin) {
              XLOG(ERR) << "Error parsing uni stream id=" << streamId
                        << " sess=" << self.get();
            }
          }
        }
      });
}

void MoQSession::onDatagram(std::unique_ptr<compat::Payload> /*datagram*/) {
  // Datagram handling - parse object header and deliver to consumer
  // TODO: Full datagram parsing and delivery
}

// =========================================================================
// Phase 2: Response Send Methods
// =========================================================================

void MoQSession::sendSubscribeOk(const SubscribeOk& subOk) {
  auto res = moqFrameWriter_.writeSubscribeOk(controlWriteBuf_, subOk);
  if (!res) {
    XLOG(ERR) << "writeSubscribeOk failed sess=" << this;
    return;
  }
  flushControlBuf();
}

void MoQSession::subscribeError(const SubscribeError& subErr) {
  auto it = pubTracks_.find(subErr.requestID);
  if (it != pubTracks_.end()) {
    pubTracks_.erase(it);
  }
  auto res = moqFrameWriter_.writeRequestError(
      controlWriteBuf_, subErr, FrameType::SUBSCRIBE_ERROR);
  retireRequestID(false);
  if (!res) {
    XLOG(ERR) << "writeSubscribeError failed sess=" << this;
    return;
  }
  flushControlBuf();
}

void MoQSession::unsubscribe(const Unsubscribe& unsub) {
  auto trackAliasIt = reqIdToTrackAlias_.find(unsub.requestID);
  if (trackAliasIt != reqIdToTrackAlias_.end()) {
    auto trackIt = subTracks_.find(trackAliasIt->second);
    if (trackIt != subTracks_.end()) {
      trackIt->second->cancel();
      subTracks_.erase(trackIt);
    }
    reqIdToTrackAlias_.erase(trackAliasIt);
  }
  auto res = moqFrameWriter_.writeUnsubscribe(controlWriteBuf_, unsub);
  if (!res) {
    XLOG(ERR) << "writeUnsubscribe failed sess=" << this;
    return;
  }
  flushControlBuf();
  checkForCloseOnDrain();
}

void MoQSession::subscribeUpdate(const SubscribeUpdate& subUpdate) {
  auto res = moqFrameWriter_.writeSubscribeUpdate(controlWriteBuf_, subUpdate);
  if (!res) {
    XLOG(ERR) << "writeSubscribeUpdate failed sess=" << this;
    return;
  }
  flushControlBuf();
}

void MoQSession::subscribeUpdateOk(const RequestOk& requestOk) {
  auto res = moqFrameWriter_.writeRequestOk(
      controlWriteBuf_, requestOk, FrameType::REQUEST_OK);
  if (!res) {
    XLOG(ERR) << "writeRequestOk failed sess=" << this;
    return;
  }
  flushControlBuf();
}

void MoQSession::subscribeUpdateError(
    const SubscribeUpdateError& requestError,
    RequestID subscriptionRequestID) {
  auto res = moqFrameWriter_.writeRequestError(
      controlWriteBuf_, requestError, FrameType::SUBSCRIBE_UPDATE);
  if (!res) {
    XLOG(ERR) << "writeSubscribeUpdateError failed sess=" << this;
  } else {
    flushControlBuf();
  }

  // Terminate subscription
  auto it = pubTracks_.find(subscriptionRequestID);
  if (it != pubTracks_.end()) {
    PublishDone pubDone{
        subscriptionRequestID,
        PublishDoneStatusCode::UPDATE_FAILED,
        static_cast<uint64_t>(requestError.errorCode),
        requestError.reasonPhrase};
    it->second->terminatePublish(pubDone, ResetStreamErrorCode::CANCELLED);
  }
}

void MoQSession::sendPublishDone(const PublishDone& pubDone) {
  auto it = pubTracks_.find(pubDone.requestID);
  if (it == pubTracks_.end()) {
    XLOG(ERR) << "publishDone for invalid id=" << pubDone.requestID
              << " sess=" << this;
    return;
  }
  pubTracks_.erase(it);
  auto res = moqFrameWriter_.writePublishDone(controlWriteBuf_, pubDone);
  if (!res) {
    XLOG(ERR) << "writePublishDone failed sess=" << this;
    return;
  }
  flushControlBuf();
  retireRequestID(false);
}

void MoQSession::fetchOk(const FetchOk& fetchOkMsg) {
  auto res = moqFrameWriter_.writeFetchOk(controlWriteBuf_, fetchOkMsg);
  if (!res) {
    XLOG(ERR) << "writeFetchOk failed sess=" << this;
    return;
  }
  flushControlBuf();
}

void MoQSession::fetchError(const FetchError& fetchErr) {
  auto it = pubTracks_.find(fetchErr.requestID);
  if (it != pubTracks_.end()) {
    pubTracks_.erase(it);
  }
  auto res = moqFrameWriter_.writeRequestError(
      controlWriteBuf_, fetchErr, FrameType::FETCH_ERROR);
  retireRequestID(false);
  if (!res) {
    XLOG(ERR) << "writeFetchError failed sess=" << this;
    return;
  }
  flushControlBuf();
}

void MoQSession::fetchCancel(const FetchCancel& fetchCan) {
  auto res = moqFrameWriter_.writeFetchCancel(controlWriteBuf_, fetchCan);
  if (!res) {
    XLOG(ERR) << "writeFetchCancel failed sess=" << this;
    return;
  }
  flushControlBuf();
}

void MoQSession::publishOk(const PublishOk& pubOk) {
  auto res = moqFrameWriter_.writePublishOk(controlWriteBuf_, pubOk);
  if (!res) {
    XLOG(ERR) << "writePublishOk failed sess=" << this;
    return;
  }
  flushControlBuf();
}

void MoQSession::publishError(const PublishError& pubErr) {
  auto res = moqFrameWriter_.writeRequestError(
      controlWriteBuf_, pubErr, FrameType::PUBLISH_ERROR);
  if (!res) {
    XLOG(ERR) << "writePublishError failed sess=" << this;
    return;
  }
  flushControlBuf();
}

void MoQSession::trackStatusOk(const TrackStatusOk& tsOk) {
  auto res = moqFrameWriter_.writeTrackStatusOk(controlWriteBuf_, tsOk);
  if (!res) {
    XLOG(ERR) << "writeTrackStatusOk failed sess=" << this;
    return;
  }
  flushControlBuf();
}

void MoQSession::trackStatusError(const TrackStatusError& tsErr) {
  auto res = moqFrameWriter_.writeTrackStatusError(controlWriteBuf_, tsErr);
  if (!res) {
    XLOG(ERR) << "writeTrackStatusError failed sess=" << this;
    return;
  }
  flushControlBuf();
}

void MoQSession::publishNamespaceError(
    const PublishNamespaceError& publishNamespaceErr) {
  auto res = moqFrameWriter_.writeRequestError(
      controlWriteBuf_, publishNamespaceErr,
      FrameType::PUBLISH_NAMESPACE_ERROR);
  if (!res) {
    XLOG(ERR) << "writePublishNamespaceError failed sess=" << this;
    return;
  }
  flushControlBuf();
}

void MoQSession::subscribeNamespaceError(
    const SubscribeNamespaceError& subscribeNamespaceErr) {
  auto res = moqFrameWriter_.writeRequestError(
      controlWriteBuf_, subscribeNamespaceErr,
      FrameType::SUBSCRIBE_NAMESPACE_ERROR);
  if (!res) {
    XLOG(ERR) << "writeSubscribeNamespaceError failed sess=" << this;
    return;
  }
  flushControlBuf();
}

void MoQSession::sendMaxRequestID(bool /*signalWriteLoop*/) {
  auto res = moqFrameWriter_.writeMaxRequestID(
      controlWriteBuf_, {.requestID = maxRequestID_});
  if (!res) {
    XLOG(ERR) << "writeMaxRequestID failed sess=" << this;
    return;
  }
  flushControlBuf();
}

// =========================================================================
// MoQControlCodec::ControlCallback implementations
// =========================================================================

void MoQSession::onClientSetup(ClientSetup clientSetup) {
  if (dir_ != MoQControlCodec::Direction::SERVER) {
    XLOG(ERR) << "Received CLIENT_SETUP on client";
    close(SessionCloseErrorCode::PROTOCOL_VIOLATION);
    return;
  }

  if (!serverSetupCallback_) {
    XLOG(ERR) << "No server setup callback";
    close(SessionCloseErrorCode::INTERNAL_ERROR);
    return;
  }

  peerMaxRequestID_ = getMaxRequestIDIfPresent(clientSetup.params);

  auto result = serverSetupCallback_->onClientSetup(
      std::move(clientSetup), shared_from_this());

  if (result.hasValue()) {
    auto& serverSetup = result.value();

    if (!negotiatedVersion_) {
      initializeNegotiatedVersion(serverSetup.selectedVersion);
    }

    auto writeRes =
        writeServerSetup(controlWriteBuf_, serverSetup, *negotiatedVersion_);
    if (writeRes.hasError()) {
      XLOG(ERR) << "Failed to write SERVER_SETUP";
      close(SessionCloseErrorCode::INTERNAL_ERROR);
      return;
    }
    setupComplete_ = true;
    flushControlBuf();
  } else {
    XLOG(ERR) << "Server setup callback failed";
    close(SessionCloseErrorCode::INTERNAL_ERROR);
  }
}

void MoQSession::onServerSetup(ServerSetup setup) {
  if (dir_ != MoQControlCodec::Direction::CLIENT) {
    XLOG(ERR) << "Received SERVER_SETUP on server";
    close(SessionCloseErrorCode::PROTOCOL_VIOLATION);
    return;
  }

  if (!negotiatedVersion_) {
    initializeNegotiatedVersion(setup.selectedVersion);
  }

  peerMaxRequestID_ = getMaxRequestIDIfPresent(setup.params);
  setupComplete_ = true;

  if (setupCallback_) {
    setupCallback_->onSuccess(std::move(setup));
    setupCallback_.reset();
  }
}

// =========================================================================
// Phase 3: Server-side onSubscribe
// =========================================================================

void MoQSession::onSubscribe(SubscribeRequest subscribeRequest) {
  const auto requestID = subscribeRequest.requestID;

  if (closeSessionIfRequestIDInvalid(requestID, false, true)) {
    return;
  }

  if (receivedGoaway_) {
    subscribeError(
        {subscribeRequest.requestID,
         SubscribeErrorCode::GOING_AWAY,
         "Session received GOAWAY"});
    return;
  }

  if (!publishHandler_) {
    subscribeError(
        {subscribeRequest.requestID,
         SubscribeErrorCode::NOT_SUPPORTED,
         "No publish handler"});
    return;
  }

  auto it = pubTracks_.find(subscribeRequest.requestID);
  if (it != pubTracks_.end()) {
    subscribeError(
        {subscribeRequest.requestID,
         SubscribeErrorCode::INTERNAL_ERROR,
         "dup sub ID"});
    return;
  }

  auto trackPublisher = std::make_shared<CompatTrackPublisher>(
      this,
      wt_.get(),
      subscribeRequest.fullTrackName,
      subscribeRequest.requestID,
      subscribeRequest.priority,
      subscribeRequest.groupOrder,
      *negotiatedVersion_);

  pubTracks_.emplace(requestID, trackPublisher);

  // Call publish handler asynchronously via callback
  auto self = shared_from_this();
  auto trackConsumer =
      std::static_pointer_cast<TrackConsumer>(trackPublisher);

  auto resultCallback = compat::makeResultCallback<
      std::shared_ptr<SubscriptionHandle>, SubscribeError>(
      // onSuccess
      [self, requestID, trackPublisher](
          std::shared_ptr<SubscriptionHandle> handle) {
        auto subOk = handle->subscribeOk();
        subOk.requestID = requestID;
        trackPublisher->subscribeOkSent(subOk);
        self->sendSubscribeOk(subOk);
        trackPublisher->setSubscriptionHandle(std::move(handle));
      },
      // onError
      [self, requestID](SubscribeError subErr) {
        subErr.requestID = requestID;
        self->subscribeError(subErr);
      });

  publishHandler_->subscribeWithCallback(
      std::move(subscribeRequest),
      std::move(trackConsumer),
      std::move(resultCallback));
}

// =========================================================================
// Phase 4: Client-side subscribe
// =========================================================================

void MoQSession::subscribeWithCallback(
    SubscribeRequest sub,
    std::shared_ptr<TrackConsumer> consumer,
    std::shared_ptr<compat::ResultCallback<
        std::shared_ptr<SubscriptionHandle>,
        SubscribeError>> callback) {
  if (draining_) {
    callback->onError(SubscribeError{
        RequestID(0), SubscribeErrorCode::INTERNAL_ERROR, "Draining session"});
    return;
  }

  auto fullTrackName = sub.fullTrackName;
  RequestID reqID = getNextRequestID();
  sub.requestID = reqID;
  TrackAlias trackAlias{reqID.value};

  auto wres = moqFrameWriter_.writeSubscribeRequest(controlWriteBuf_, sub);
  if (!wres) {
    XLOG(ERR) << "writeSubscribeRequest failed sess=" << this;
    callback->onError(SubscribeError{
        reqID, SubscribeErrorCode::INTERNAL_ERROR, "local write failed"});
    return;
  }
  flushControlBuf();

  auto trackReceiveState = std::make_shared<SubscribeTrackReceiveState>(
      fullTrackName, reqID, consumer, this, trackAlias);

  pendingSubscribes_.emplace(
      reqID,
      CompatPendingSubscribe{trackReceiveState, consumer, callback});
}

void MoQSession::onSubscribeOk(SubscribeOk subOk) {
  auto it = pendingSubscribes_.find(subOk.requestID);
  if (it == pendingSubscribes_.end()) {
    XLOG(ERR) << "No matching subscribe ID=" << subOk.requestID
              << " sess=" << this;
    close(SessionCloseErrorCode::PROTOCOL_VIOLATION);
    return;
  }

  auto pending = std::move(it->second);
  pendingSubscribes_.erase(it);

  auto trackAlias = subOk.trackAlias;
  auto res = reqIdToTrackAlias_.try_emplace(subOk.requestID, trackAlias);
  if (!res.second) {
    XLOG(ERR) << "Request ID already mapped reqID=" << subOk.requestID
              << " sess=" << this;
    close(SessionCloseErrorCode::PROTOCOL_VIOLATION);
    return;
  }

  auto emplaceRes = subTracks_.try_emplace(trackAlias, pending.trackState);
  if (!emplaceRes.second) {
    XLOG(ERR) << "TrackAlias already in use " << trackAlias
              << " sess=" << this;
    close(SessionCloseErrorCode::DUPLICATE_TRACK_ALIAS);
    return;
  }

  auto handle = std::make_shared<CompatReceiverSubscriptionHandle>(
      std::move(subOk), trackAlias, shared_from_this());

  if (pending.callback) {
    pending.callback->onSuccess(std::move(handle));
  }

  deliverBufferedData(trackAlias);
}

void MoQSession::onRequestOk(RequestOk /*requestOk*/, FrameType /*frameType*/) {
  // Handle REQUEST_OK for subscribe updates etc.
  // For now, minimal handling
}

void MoQSession::onRequestError(
    RequestError requestError,
    FrameType frameType) {
  switch (frameType) {
    case FrameType::SUBSCRIBE_ERROR: {
      auto it = pendingSubscribes_.find(requestError.requestID);
      if (it != pendingSubscribes_.end()) {
        auto pending = std::move(it->second);
        pendingSubscribes_.erase(it);
        if (pending.callback) {
          pending.callback->onError(
              static_cast<const SubscribeError&>(requestError));
        }
      }
      break;
    }
    case FrameType::FETCH_ERROR: {
      auto it = pendingFetches_.find(requestError.requestID);
      if (it != pendingFetches_.end()) {
        auto pending = std::move(it->second);
        pendingFetches_.erase(it);
        if (pending.callback) {
          pending.callback->onError(
              static_cast<const FetchError&>(requestError));
        }
      }
      break;
    }
    case FrameType::PUBLISH_NAMESPACE_ERROR:
    case FrameType::SUBSCRIBE_NAMESPACE_ERROR:
    case FrameType::PUBLISH_ERROR:
      // Handle other error types as needed
      break;
    default:
      break;
  }
}

// =========================================================================
// Phase 6: Fetch flows
// =========================================================================

void MoQSession::onFetch(Fetch fetch) {
  auto [standalone, joining] = fetchType(fetch);
  const auto requestID = fetch.requestID;

  if (closeSessionIfRequestIDInvalid(requestID, false, true)) {
    return;
  }

  if (receivedGoaway_) {
    fetchError({fetch.requestID, FetchErrorCode::GOING_AWAY,
                "Session received GOAWAY"});
    return;
  }

  if (!publishHandler_) {
    fetchError({fetch.requestID, FetchErrorCode::NOT_SUPPORTED,
                "No publish handler"});
    return;
  }

  if (standalone) {
    if (standalone->end <= standalone->start) {
      if (!(standalone->end.group == standalone->start.group &&
            standalone->end.object == 0)) {
        fetchError({fetch.requestID, FetchErrorCode::INVALID_RANGE,
                    "End must be after start"});
        return;
      }
    }
  } else if (joining) {
    auto joinIt = pubTracks_.find(joining->joiningRequestID);
    if (joinIt == pubTracks_.end()) {
      fetchError({fetch.requestID, FetchErrorCode::INTERNAL_ERROR,
                  "Unknown joining requestID"});
      return;
    }
    fetch.fullTrackName = joinIt->second->fullTrackName();
  }

  auto it = pubTracks_.find(fetch.requestID);
  if (it != pubTracks_.end()) {
    fetchError({fetch.requestID, FetchErrorCode::INTERNAL_ERROR, "dup sub ID"});
    return;
  }

  auto fetchPublisher = std::make_shared<CompatFetchPublisher>(
      this,
      wt_.get(),
      fetch.fullTrackName,
      fetch.requestID,
      fetch.priority,
      fetch.groupOrder,
      *negotiatedVersion_);

  auto streamPublisher = fetchPublisher->getStreamPublisher();
  if (!streamPublisher) {
    fetchError({fetch.requestID, FetchErrorCode::INTERNAL_ERROR,
                "Failed to create fetch stream"});
    return;
  }

  pubTracks_.emplace(fetch.requestID, fetchPublisher);

  auto self = shared_from_this();
  auto resultCallback = compat::makeResultCallback<
      std::shared_ptr<Publisher::FetchHandle>, FetchError>(
      // onSuccess
      [self, requestID, fetchPublisher](
          std::shared_ptr<Publisher::FetchHandle> handle) {
        if (fetchPublisher->isCancelled()) {
          return;
        }
        auto fetchOkMsg = handle->fetchOk();
        fetchOkMsg.requestID = requestID;
        self->fetchOk(fetchOkMsg);
      },
      // onError
      [self, requestID, fetchPublisher](FetchError fetchErr) {
        fetchPublisher->reset(ResetStreamErrorCode::INTERNAL_ERROR);
        fetchErr.requestID = requestID;
        self->fetchError(fetchErr);
      });

  publishHandler_->fetchWithCallback(
      std::move(fetch),
      std::move(streamPublisher),
      std::move(resultCallback));
}

void MoQSession::fetchWithCallback(
    Fetch fetch,
    std::shared_ptr<FetchConsumer> consumer,
    std::shared_ptr<
        compat::ResultCallback<std::shared_ptr<Publisher::FetchHandle>,
                               FetchError>> callback) {
  if (draining_) {
    callback->onError(FetchError{
        RequestID(0), FetchErrorCode::INTERNAL_ERROR, "Draining session"});
    return;
  }

  auto fullTrackName = fetch.fullTrackName;
  RequestID reqID = getNextRequestID();
  fetch.requestID = reqID;

  auto wres = moqFrameWriter_.writeFetch(controlWriteBuf_, fetch);
  if (!wres) {
    XLOG(ERR) << "writeFetch failed sess=" << this;
    callback->onError(FetchError{
        reqID, FetchErrorCode::INTERNAL_ERROR, "local write failed"});
    return;
  }
  flushControlBuf();

  auto fetchReceiveState = std::make_shared<FetchTrackReceiveState>(
      fullTrackName, reqID, consumer);

  pendingFetches_.emplace(
      reqID,
      CompatPendingFetch{fetchReceiveState, consumer, callback});
}

void MoQSession::onFetchOk(FetchOk fetchOkMsg) {
  auto it = pendingFetches_.find(fetchOkMsg.requestID);
  if (it == pendingFetches_.end()) {
    XLOG(ERR) << "No matching fetch ID=" << fetchOkMsg.requestID
              << " sess=" << this;
    close(SessionCloseErrorCode::PROTOCOL_VIOLATION);
    return;
  }

  auto pending = std::move(it->second);
  pendingFetches_.erase(it);

  pending.fetchState->fetchOK(fetchOkMsg);
  fetches_.emplace(fetchOkMsg.requestID, pending.fetchState);

  auto handle = std::make_shared<CompatReceiverFetchHandle>(
      std::move(fetchOkMsg), shared_from_this());

  if (pending.callback) {
    pending.callback->onSuccess(std::move(handle));
  }
}

void MoQSession::onFetchCancel(FetchCancel fetchCan) {
  auto it = pubTracks_.find(fetchCan.requestID);
  if (it != pubTracks_.end()) {
    it->second->terminatePublish(
        PublishDone{fetchCan.requestID, PublishDoneStatusCode::SUBSCRIPTION_ENDED,
                      0, "fetch cancelled"},
        ResetStreamErrorCode::CANCELLED);
    pubTracks_.erase(it);
  }
}

// =========================================================================
// Phase 7: Remaining handlers
// =========================================================================

void MoQSession::onRequestUpdate(RequestUpdate requestUpdate) {
  auto subscriptionRequestID = requestUpdate.requestID;
  auto it = pubTracks_.find(subscriptionRequestID);
  if (it == pubTracks_.end()) {
    XLOG(ERR) << "No matching subscribe ID=" << subscriptionRequestID
              << " sess=" << this;
    return;
  }
  it->second->setSubPriority(requestUpdate.priority);
}

void MoQSession::onUnsubscribe(Unsubscribe unsub) {
  auto it = pubTracks_.find(unsub.requestID);
  if (it != pubTracks_.end()) {
    it->second->terminatePublish(
        PublishDone{unsub.requestID, PublishDoneStatusCode::SUBSCRIPTION_ENDED,
                      0, "unsubscribed"},
        ResetStreamErrorCode::CANCELLED);
    pubTracks_.erase(it);
  }
  retireRequestID(true);
}

void MoQSession::onPublishDone(PublishDone publishDone) {
  auto aliasIt = reqIdToTrackAlias_.find(publishDone.requestID);
  if (aliasIt == reqIdToTrackAlias_.end()) {
    XLOG(ERR) << "No matching request ID=" << publishDone.requestID
              << " sess=" << this;
    return;
  }
  auto trackIt = subTracks_.find(aliasIt->second);
  if (trackIt == subTracks_.end()) {
    XLOG(ERR) << "No matching track alias=" << aliasIt->second
              << " sess=" << this;
    return;
  }
  trackIt->second->processPublishDone(std::move(publishDone));
}

void MoQSession::onPublish(PublishRequest publish) {
  if (!subscribeHandler_) {
    publishError(PublishError{
        publish.requestID, PublishErrorCode::NOT_SUPPORTED, "Not a subscriber"});
    return;
  }

  auto requestID = publish.requestID;
  auto alias = publish.trackAlias;
  auto ftn = publish.fullTrackName;

  auto result = subscribeHandler_->publish(std::move(publish));
  if (result.hasError()) {
    publishError(result.error());
    return;
  }

  auto& value = result.value();
  auto consumer = std::move(value.consumer);

  if (consumer) {
    // Store track receive state for incoming data
    reqIdToTrackAlias_.emplace(requestID, alias);
    auto trackReceiveState = std::make_shared<SubscribeTrackReceiveState>(
        ftn, requestID, consumer, this, alias);
    consumer->setTrackAlias(alias);
    subTracks_.emplace(alias, trackReceiveState);
  }

  // Send PUBLISH_OK
  PublishOk pubOk;
  pubOk.requestID = requestID;
  publishOk(pubOk);
}

void MoQSession::onPublishOk(PublishOk /*publishOk*/) {
  // Handle PUBLISH_OK - resolve pending publish requests
}

void MoQSession::onMaxRequestID(MaxRequestID maxSubId) {
  peerMaxRequestID_ = maxSubId.requestID.value;
}

void MoQSession::onRequestsBlocked(RequestsBlocked /*requestsBlocked*/) {
  // TODO: send MAX_REQUEST_ID if possible
}

void MoQSession::onTrackStatus(TrackStatus trackStatus) {
  if (!publishHandler_) {
    trackStatusError(TrackStatusError{
        trackStatus.requestID,
        TrackStatusErrorCode::NOT_SUPPORTED,
        "No publish handler"});
    return;
  }

  auto self = shared_from_this();
  auto resultCallback = compat::makeResultCallback<TrackStatusOk, TrackStatusError>(
      [self](TrackStatusOk ok) {
        self->trackStatusOk(ok);
      },
      [self](TrackStatusError err) {
        self->trackStatusError(err);
      });

  publishHandler_->trackStatusWithCallback(trackStatus, std::move(resultCallback));
}

void MoQSession::onTrackStatusOk(TrackStatusOk /*trackStatusOk*/) {
  // Client-side: resolve pending track status requests
}

void MoQSession::onTrackStatusError(TrackStatusError /*trackStatusError*/) {
  // Client-side: resolve pending track status error
}

void MoQSession::onGoaway(Goaway goway) {
  receivedGoaway_ = true;
  if (subscribeHandler_) {
    subscribeHandler_->goaway(std::move(goway));
  }
}

void MoQSession::onConnectionError(ErrorCode error) {
  XLOG(ERR) << "Connection error: " << static_cast<uint32_t>(error);
  close(SessionCloseErrorCode::INTERNAL_ERROR);
}

void MoQSession::onPublishNamespace(PublishNamespace publishNamespace) {
  if (!subscribeHandler_) {
    publishNamespaceError(PublishNamespaceError{
        publishNamespace.requestID,
        PublishNamespaceErrorCode::NOT_SUPPORTED,
        "No subscriber handler"});
    return;
  }

  auto self = shared_from_this();
  auto resultCallback = compat::makeResultCallback<
      std::shared_ptr<Subscriber::PublishNamespaceHandle>,
      PublishNamespaceError>(
      [self, requestID = publishNamespace.requestID](
          std::shared_ptr<Subscriber::PublishNamespaceHandle> handle) {
        auto ok = handle->publishNamespaceOk();
        auto res = self->moqFrameWriter_.writePublishNamespaceOk(
            self->controlWriteBuf_, ok);
        if (res) {
          self->flushControlBuf();
        }
      },
      [self](PublishNamespaceError err) {
        self->publishNamespaceError(err);
      });

  subscribeHandler_->publishNamespaceWithCallback(
      std::move(publishNamespace), nullptr, std::move(resultCallback));
}

void MoQSession::onPublishNamespaceDone(PublishNamespaceDone /*publishNamespaceDone*/) {
  // Handle announce done
}

void MoQSession::onPublishNamespaceCancel(PublishNamespaceCancel /*publishNamespaceCancel*/) {
  // Handle announce cancel
}

void MoQSession::onSubscribeNamespace(SubscribeNamespace subscribeNamespace) {
  if (!publishHandler_) {
    subscribeNamespaceError(SubscribeNamespaceError{
        subscribeNamespace.requestID,
        SubscribeNamespaceErrorCode::NOT_SUPPORTED,
        "No publish handler"});
    return;
  }

  auto self = shared_from_this();
  auto resultCallback = compat::makeResultCallback<
      std::shared_ptr<Publisher::SubscribeNamespaceHandle>,
      SubscribeNamespaceError>(
      [self](std::shared_ptr<Publisher::SubscribeNamespaceHandle> handle) {
        auto ok = handle->subscribeNamespaceOk();
        auto res = self->moqFrameWriter_.writeSubscribeNamespaceOk(
            self->controlWriteBuf_, ok);
        if (res) {
          self->flushControlBuf();
        }
      },
      [self](SubscribeNamespaceError err) {
        self->subscribeNamespaceError(err);
      });

  publishHandler_->subscribeNamespaceWithCallback(
      std::move(subscribeNamespace), std::move(resultCallback));
}

void MoQSession::onUnsubscribeNamespace(UnsubscribeNamespace /*unsubscribeNamespace*/) {
  // Handle unsubscribe namespace
}

} // namespace moxygen

#endif // !MOXYGEN_QUIC_MVFST
