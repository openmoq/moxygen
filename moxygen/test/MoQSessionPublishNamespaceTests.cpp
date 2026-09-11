/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "moxygen/test/MoQSessionTestCommon.h"

using namespace moxygen;
using namespace moxygen::test;
using testing::_;

// === PUBLISH_NAMESPACE tests ===

CO_TEST_P_X(MoQSessionTest, PublishNamespace) {
  co_await setupMoQSession();

  EXPECT_CALL(*serverSubscriber, publishNamespace(_, _))
      .WillOnce(
          testing::Invoke(
              [](auto ann, auto /* publishNamespaceCallback */)
                  -> folly::coro::Task<Subscriber::PublishNamespaceResult> {
                co_return makePublishNamespaceOkResult(ann);
              }));

  EXPECT_CALL(*clientPublisherStatsCallback_, onPublishNamespaceSuccess());
  EXPECT_CALL(*serverSubscriberStatsCallback_, onPublishNamespaceSuccess());
  EXPECT_CALL(*clientPublisherStatsCallback_, recordPublishNamespaceLatency(_));
  auto publishNamespaceResult =
      co_await clientSession_->publishNamespace(getPublishNamespace());
  EXPECT_FALSE(publishNamespaceResult.hasError());
  co_await folly::coro::co_reschedule_on_current_executor;
  clientSession_->close(SessionCloseErrorCode::NO_ERROR);
}
CO_TEST_P_X(MoQSessionTest, PublishNamespaceDone) {
  co_await setupMoQSession();

  std::shared_ptr<MockPublishNamespaceHandle> mockPublishNamespaceHandle;
  EXPECT_CALL(*serverSubscriber, publishNamespace(_, _))
      .WillOnce(
          testing::Invoke(
              [&mockPublishNamespaceHandle](
                  auto ann, auto /* publishNamespaceCallback */)
                  -> folly::coro::Task<Subscriber::PublishNamespaceResult> {
                mockPublishNamespaceHandle =
                    std::make_shared<MockPublishNamespaceHandle>(
                        PublishNamespaceOk(
                            {.requestID = ann.requestID,
                             .requestSpecificParams = {}}));
                Subscriber::PublishNamespaceResult publishNamespaceResult(
                    mockPublishNamespaceHandle);
                co_return publishNamespaceResult;
              }));

  EXPECT_CALL(*clientPublisherStatsCallback_, onPublishNamespaceSuccess());
  EXPECT_CALL(*serverSubscriberStatsCallback_, onPublishNamespaceSuccess());
  auto publishNamespaceResult =
      co_await clientSession_->publishNamespace(getPublishNamespace());
  EXPECT_FALSE(publishNamespaceResult.hasError());
  auto publishNamespaceHandle = publishNamespaceResult.value();
  EXPECT_CALL(*clientPublisherStatsCallback_, onPublishNamespaceDone());
  EXPECT_CALL(*serverSubscriberStatsCallback_, onPublishNamespaceDone());

  folly::coro::Baton barricade;
  EXPECT_CALL(*mockPublishNamespaceHandle, publishNamespaceDone())
      .WillOnce(testing::Invoke([&barricade]() { barricade.post(); }));
  publishNamespaceHandle->publishNamespaceDone();
  co_await barricade;
  clientSession_->close(SessionCloseErrorCode::NO_ERROR);
}
CO_TEST_P_X(MoQSessionTest, PublishNamespaceCancel) {
  co_await setupMoQSession();

  std::shared_ptr<MockPublishNamespaceHandle> mockPublishNamespaceHandle;
  std::shared_ptr<moxygen::Subscriber::PublishNamespaceCallback>
      publishNamespaceCallback;
  EXPECT_CALL(*serverSubscriber, publishNamespace(_, _))
      .WillOnce(
          testing::Invoke(
              [&mockPublishNamespaceHandle, &publishNamespaceCallback](
                  auto ann, auto publishNamespaceCallbackIn)
                  -> folly::coro::Task<Subscriber::PublishNamespaceResult> {
                publishNamespaceCallback = publishNamespaceCallbackIn;
                mockPublishNamespaceHandle =
                    std::make_shared<MockPublishNamespaceHandle>(
                        PublishNamespaceOk(
                            {.requestID = ann.requestID,
                             .requestSpecificParams = {}}));
                Subscriber::PublishNamespaceResult publishNamespaceResult(
                    mockPublishNamespaceHandle);
                co_return publishNamespaceResult;
              }));

  EXPECT_CALL(*clientPublisherStatsCallback_, onPublishNamespaceSuccess());
  EXPECT_CALL(*serverSubscriberStatsCallback_, onPublishNamespaceSuccess());
  auto mockPublishNamespaceCallback =
      std::make_shared<MockPublishNamespaceCallback>();
  auto publishNamespaceResult = co_await clientSession_->publishNamespace(
      getPublishNamespace(), mockPublishNamespaceCallback);
  EXPECT_FALSE(publishNamespaceResult.hasError());
  EXPECT_CALL(*clientPublisherStatsCallback_, onPublishNamespaceCancel());
  EXPECT_CALL(*serverSubscriberStatsCallback_, onPublishNamespaceCancel());

  folly::coro::Baton barricade;
  EXPECT_CALL(*mockPublishNamespaceCallback, publishNamespaceCancel(_, _))
      .WillOnce(
          testing::Invoke(
              [&barricade](moxygen::PublishNamespaceErrorCode, std::string) {
                barricade.post();
                return;
              }));
  publishNamespaceCallback->publishNamespaceCancel(
      PublishNamespaceErrorCode::UNINTERESTED, "Not interested!");

  co_await barricade;
  clientSession_->close(SessionCloseErrorCode::NO_ERROR);
}
// Draft 18+ subscriber-initiated withdrawal: subscriber tears down the
// PUBLISH_NAMESPACE bidi, the peer's read loop synthesizes
// onPublishNamespaceDone. Mirror of the publisher-initiated path above.
CO_TEST_P_X(Draft18Test, SubscriberCancelsPublishNamespace) {
  co_await setupMoQSession();

  std::shared_ptr<MockPublishNamespaceHandle> mockPublishNamespaceHandle;
  EXPECT_CALL(*serverSubscriber, publishNamespace(_, _))
      .WillOnce(
          [&mockPublishNamespaceHandle](
              auto ann, auto /* publishNamespaceCallback */)
              -> folly::coro::Task<Subscriber::PublishNamespaceResult> {
            mockPublishNamespaceHandle =
                std::make_shared<MockPublishNamespaceHandle>(PublishNamespaceOk(
                    {.requestID = ann.requestID, .requestSpecificParams = {}}));
            co_return Subscriber::PublishNamespaceResult(
                mockPublishNamespaceHandle);
          });

  EXPECT_CALL(*clientPublisherStatsCallback_, onPublishNamespaceSuccess());
  EXPECT_CALL(*serverSubscriberStatsCallback_, onPublishNamespaceSuccess());
  auto publishNamespaceResult =
      co_await clientSession_->publishNamespace(getPublishNamespace());
  EXPECT_FALSE(publishNamespaceResult.hasError());

  // STOP_SENDING the bidi read half → server fires its close callback,
  // synthesizing onPublishNamespaceDone.
  folly::coro::Baton doneBaton;
  EXPECT_CALL(*serverSubscriberStatsCallback_, onPublishNamespaceDone());
  EXPECT_CALL(*mockPublishNamespaceHandle, publishNamespaceDone())
      .WillOnce([&] { doneBaton.post(); });
  serverWt_->readHandles.at(0)->stopSending(
      folly::to_underlying(ResetStreamErrorCode::CANCELLED));
  co_await doneBaton;

  clientSession_->close(SessionCloseErrorCode::NO_ERROR);
}

CO_TEST_P_X(MoQSessionTest, PublishNamespaceError) {
  co_await setupMoQSession();

  EXPECT_CALL(*serverSubscriber, publishNamespace(_, _))
      .WillOnce(
          testing::Invoke(
              [](auto ann, auto /* publishNamespaceCallback */)
                  -> folly::coro::Task<Subscriber::PublishNamespaceResult> {
                co_return folly::makeUnexpected(
                    PublishNamespaceError{
                        ann.requestID,
                        PublishNamespaceErrorCode::UNAUTHORIZED,
                        "Unauthorized"});
              }));

  EXPECT_CALL(
      *clientPublisherStatsCallback_,
      onPublishNamespaceError(PublishNamespaceErrorCode::UNAUTHORIZED));
  EXPECT_CALL(
      *serverSubscriberStatsCallback_,
      onPublishNamespaceError(PublishNamespaceErrorCode::UNAUTHORIZED));

  auto publishNamespaceResult =
      co_await clientSession_->publishNamespace(getPublishNamespace());
  EXPECT_TRUE(publishNamespaceResult.hasError());
  EXPECT_EQ(
      publishNamespaceResult.error().errorCode,
      PublishNamespaceErrorCode::UNAUTHORIZED);

  clientSession_->close(SessionCloseErrorCode::NO_ERROR);
}

// Sender: peer FINs the PUBLISH_NAMESPACE bidi before REQUEST_OK/ERROR —
// publishNamespace must fail rather than strand.
CO_TEST_P_X(Draft18Test, PublishNamespaceFailsOnPeerFinWithoutReply) {
  co_await setupMoQSession();

  folly::coro::Baton serverSawAnn;
  folly::coro::Baton releaseHandler;
  EXPECT_CALL(*serverSubscriber, publishNamespace(_, _))
      .WillOnce(
          [&](auto ann, auto /*cb*/)
              -> folly::coro::Task<Subscriber::PublishNamespaceResult> {
            serverSawAnn.post();
            co_await releaseHandler;
            co_return makePublishNamespaceOkResult(ann);
          });

  std::optional<PublishNamespaceErrorCode> errorCode;
  folly::coro::Baton done;
  folly::coro::co_withExecutor(
      MoQExecutor_.get(),
      folly::coro::co_invoke([&]() -> folly::coro::Task<void> {
        auto result =
            co_await clientSession_->publishNamespace(getPublishNamespace());
        if (result.hasError()) {
          errorCode = result.error().errorCode;
        }
        done.post();
      }))
      .start();

  co_await serverSawAnn;
  // PUBLISH_NAMESPACE bidi is the client-initiated stream id 0.
  serverWt_->writeHandles.at(0)->writeStreamData(
      nullptr, /*fin=*/true, nullptr);

  co_await done;
  EXPECT_TRUE(errorCode.has_value());

  releaseHandler.post();
  clientSession_->close(SessionCloseErrorCode::NO_ERROR);
}

namespace {
class ClusterUpdateHandle : public Subscriber::PublishNamespaceHandle {
 public:
  explicit ClusterUpdateHandle(RequestID id)
      : PublishNamespaceHandle(PublishNamespaceOk{.requestID = id}) {}
  folly::Expected<folly::Unit, ErrorCode> publishNamespaceUpdate(
      PublishNamespace ann) override {
    latest = std::move(ann);
    received.post();
    return folly::unit;
  }
  void publishNamespaceDone() override {
    ++withdrawals;
  }
  PublishNamespace latest;
  folly::coro::Baton received;
  unsigned withdrawals{0};
};
} // namespace

CO_TEST_P_X(Draft18Test, ClusterPublishNamespaceUpdatesExistingStream) {
  relayHopsSupported_ = true;
  co_await setupMoQSession();
  std::shared_ptr<ClusterUpdateHandle> incoming;
  EXPECT_CALL(*serverSubscriber, publishNamespace(_, _))
      .Times(1)
      .WillOnce(
          [&incoming](
              auto ann,
              auto) -> folly::coro::Task<Subscriber::PublishNamespaceResult> {
            incoming = std::make_shared<ClusterUpdateHandle>(ann.requestID);
            co_return incoming;
          });
  EXPECT_CALL(*clientPublisherStatsCallback_, onPublishNamespaceSuccess())
      .Times(1);
  EXPECT_CALL(*serverSubscriberStatsCallback_, onPublishNamespaceSuccess())
      .Times(1);
  auto ann = getPublishNamespace();
  ann.params.insertParam(Parameter(
      folly::to_underlying(TrackRequestParamKey::HOP_PATH),
      *encodeRelayHopPath({42}, GetParam().serverVersion)));
  auto result = co_await clientSession_->publishNamespace(ann);
  EXPECT_TRUE(result.hasValue());
  if (!result.hasValue()) {
    co_return;
  }
  ann.params.insertParam(Parameter(0x40B58, uint64_t{7}));
  auto updated = (*result)->publishNamespaceUpdate(ann);
  EXPECT_TRUE(updated.hasValue());
  if (!updated) {
    clientSession_->close(SessionCloseErrorCode::NO_ERROR);
    co_return;
  }
  co_await incoming->received;
  EXPECT_EQ(
      incoming->latest.requestID, (*result)->publishNamespaceOk().requestID);
  EXPECT_EQ(incoming->latest.params.getFirstParam(0x40B58)->asUint64, 7);
  EXPECT_FALSE(serverSession_->isClosed());
  clientSession_->close(SessionCloseErrorCode::NO_ERROR);
}

CO_TEST_P_X(Draft18Test, ClusterRejectsSecondStreamForAdvertisedNamespace) {
  relayHopsSupported_ = true;
  co_await setupMoQSession();
  EXPECT_CALL(*serverSubscriber, publishNamespace(_, _))
      .WillRepeatedly(
          [](auto ann,
             auto) -> folly::coro::Task<Subscriber::PublishNamespaceResult> {
            co_return makePublishNamespaceOkResult(ann);
          });
  EXPECT_CALL(*clientPublisherStatsCallback_, onPublishNamespaceSuccess())
      .Times(testing::AnyNumber());
  EXPECT_CALL(*serverSubscriberStatsCallback_, onPublishNamespaceSuccess())
      .Times(testing::AnyNumber());
  auto ann = getPublishNamespace();
  ann.params.insertParam(Parameter(
      folly::to_underlying(TrackRequestParamKey::HOP_PATH),
      *encodeRelayHopPath({42}, GetParam().serverVersion)));
  auto first = co_await clientSession_->publishNamespace(ann);
  EXPECT_TRUE(first.hasValue());
  auto second = co_await clientSession_->publishNamespace(ann);
  EXPECT_TRUE(second.hasError());
  EXPECT_FALSE(serverSession_->isClosed());
  clientSession_->close(SessionCloseErrorCode::NO_ERROR);
}

CO_TEST_P_X(Draft18Test, ClusterRejectsIncomingNamespaceOnSecondStream) {
  relayHopsSupported_ = true;
  co_await setupMoQSession();
  EXPECT_CALL(*serverSubscriber, publishNamespace(_, _))
      .WillRepeatedly(
          [](auto ann,
             auto) -> folly::coro::Task<Subscriber::PublishNamespaceResult> {
            co_return makePublishNamespaceOkResult(ann);
          });
  EXPECT_CALL(*serverSubscriberStatsCallback_, onPublishNamespaceSuccess())
      .Times(testing::AnyNumber());
  auto ann = getPublishNamespace();
  ann.params.insertParam(Parameter(
      folly::to_underlying(TrackRequestParamKey::HOP_PATH),
      *encodeRelayHopPath({42}, GetParam().serverVersion)));
  MoQFrameWriter writer;
  writer.initializeVersion(
      GetParam().serverVersion, clientSession_->getNegotiatedExtensions());
  for (uint64_t id : {0, 2}) {
    auto stream = clientWt_->createBidiStream();
    EXPECT_TRUE(stream.hasValue());
    if (!stream) {
      co_return;
    }
    ann.requestID = RequestID(id);
    folly::IOBufQueue buf{folly::IOBufQueue::cacheChainLength()};
    EXPECT_TRUE(writer.writePublishNamespace(buf, ann).hasValue());
    stream->writeHandle->writeStreamData(buf.move(), false, nullptr);
    for (int i = 0; i < 50; ++i) {
      co_await folly::coro::co_reschedule_on_current_executor;
    }
  }
  EXPECT_TRUE(serverWt_->isSessionClosed());
  clientSession_->close(SessionCloseErrorCode::NO_ERROR);
}

CO_TEST_P_X(Draft18Test, ClusterQueuesUpdateUntilInitialAcceptance) {
  relayHopsSupported_ = true;
  co_await setupMoQSession();
  folly::coro::Baton started;
  folly::coro::Baton accept;
  std::shared_ptr<ClusterUpdateHandle> incoming;
  EXPECT_CALL(*serverSubscriber, publishNamespace(_, _))
      .Times(1)
      .WillOnce(
          [&](auto ann,
              auto) -> folly::coro::Task<Subscriber::PublishNamespaceResult> {
            incoming = std::make_shared<ClusterUpdateHandle>(ann.requestID);
            started.post();
            co_await accept;
            co_return incoming;
          });
  EXPECT_CALL(*serverSubscriberStatsCallback_, onPublishNamespaceSuccess())
      .Times(1);
  auto stream = clientWt_->createBidiStream();
  EXPECT_TRUE(stream.hasValue());
  if (!stream) {
    co_return;
  }
  auto ann = getPublishNamespace();
  ann.params.insertParam(Parameter(
      folly::to_underlying(TrackRequestParamKey::HOP_PATH),
      *encodeRelayHopPath({42}, kVersionDraft18)));
  MoQFrameWriter writer;
  writer.initializeVersion(
      kVersionDraft18, clientSession_->getNegotiatedExtensions());
  folly::IOBufQueue buf{folly::IOBufQueue::cacheChainLength()};
  EXPECT_TRUE(writer.writePublishNamespace(buf, ann).hasValue());
  stream->writeHandle->writeStreamData(buf.move(), false, nullptr);
  co_await started;
  ann.params.insertParam(Parameter(
      folly::to_underlying(TrackRequestParamKey::ROUTE_COST), uint64_t{9}));
  EXPECT_TRUE(writer.writePublishNamespace(buf, ann).hasValue());
  stream->writeHandle->writeStreamData(buf.move(), false, nullptr);
  for (int i = 0; i < 50; ++i) {
    co_await folly::coro::co_reschedule_on_current_executor;
  }
  EXPECT_EQ(incoming->withdrawals, 0);
  accept.post();
  co_await incoming->received;
  EXPECT_EQ(
      incoming->latest.params.getFirstParam(TrackRequestParamKey::ROUTE_COST)
          ->asUint64,
      9);
  EXPECT_EQ(incoming->withdrawals, 0);
  EXPECT_FALSE(serverSession_->isClosed());
  clientSession_->close(SessionCloseErrorCode::NO_ERROR);
}

CO_TEST_P_X(Draft18Test, ClusterFinBeforeAcceptanceCannotReviveAdvertisement) {
  relayHopsSupported_ = true;
  co_await setupMoQSession();
  folly::coro::Baton started;
  folly::coro::Baton accept;
  std::shared_ptr<ClusterUpdateHandle> incoming;
  EXPECT_CALL(*serverSubscriber, publishNamespace(_, _))
      .Times(1)
      .WillOnce(
          [&](auto ann,
              auto) -> folly::coro::Task<Subscriber::PublishNamespaceResult> {
            incoming = std::make_shared<ClusterUpdateHandle>(ann.requestID);
            started.post();
            co_await accept;
            co_return incoming;
          });
  EXPECT_CALL(*serverSubscriberStatsCallback_, onPublishNamespaceSuccess())
      .Times(0);
  auto stream = clientWt_->createBidiStream();
  EXPECT_TRUE(stream.hasValue());
  if (!stream) {
    co_return;
  }
  auto ann = getPublishNamespace();
  ann.params.insertParam(Parameter(
      folly::to_underlying(TrackRequestParamKey::HOP_PATH),
      *encodeRelayHopPath({42}, kVersionDraft18)));
  MoQFrameWriter writer;
  writer.initializeVersion(
      kVersionDraft18, clientSession_->getNegotiatedExtensions());
  folly::IOBufQueue buf{folly::IOBufQueue::cacheChainLength()};
  EXPECT_TRUE(writer.writePublishNamespace(buf, ann).hasValue());
  stream->writeHandle->writeStreamData(buf.move(), false, nullptr);
  co_await started;
  stream->writeHandle->writeStreamData(nullptr, true, nullptr);
  for (int i = 0; i < 50; ++i) {
    co_await folly::coro::co_reschedule_on_current_executor;
  }
  accept.post();
  for (int i = 0; i < 50; ++i) {
    co_await folly::coro::co_reschedule_on_current_executor;
  }
  EXPECT_EQ(incoming->withdrawals, 1);
  EXPECT_FALSE(serverSession_->isClosed());
  clientSession_->close(SessionCloseErrorCode::NO_ERROR);
}
