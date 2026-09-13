/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "moxygen/test/MoQSessionTestCommon.h"

using namespace moxygen;
using namespace moxygen::test;
using testing::_;

// === SUBSCRIBE PUBLISH_NAMESPACES tests ===

CO_TEST_P_X(MoQSessionTest, SubscribeAndUnsubscribeNamespace) {
  co_await setupMoQSession();

  std::shared_ptr<MockSubscribeNamespaceHandle> mockSubscribeNamespaceHandle;
  EXPECT_CALL(*serverPublisher, subscribeNamespace(_, _))
      .WillOnce(
          testing::Invoke(
              [&mockSubscribeNamespaceHandle](auto subAnn, auto handler)
                  -> folly::coro::Task<Publisher::SubscribeNamespaceResult> {
                mockSubscribeNamespaceHandle =
                    std::make_shared<MockSubscribeNamespaceHandle>(
                        SubscribeNamespaceOk(
                            {.requestID = RequestID(0),
                             .requestSpecificParams = {}}));
                co_return mockSubscribeNamespaceHandle;
              }));

  EXPECT_CALL(*clientSubscriberStatsCallback_, onSubscribeNamespaceSuccess());
  EXPECT_CALL(*serverPublisherStatsCallback_, onSubscribeNamespaceSuccess());
  auto publishNamespaceResult = co_await clientSession_->subscribeNamespace(
      getSubscribeNamespace(), nullptr);
  EXPECT_FALSE(publishNamespaceResult.hasError());

  EXPECT_CALL(*clientSubscriberStatsCallback_, onUnsubscribeNamespace());
  EXPECT_CALL(*serverPublisherStatsCallback_, onUnsubscribeNamespace());

  folly::coro::Baton barricade;
  EXPECT_CALL(*mockSubscribeNamespaceHandle, unsubscribeNamespace())
      .WillOnce(testing::Invoke([&barricade]() { barricade.post(); }));
  publishNamespaceResult.value()->unsubscribeNamespace();
  co_await barricade;
  clientSession_->close(SessionCloseErrorCode::NO_ERROR);
}
CO_TEST_P_X(MoQSessionTest, UnsubscribeNamespaceAfterSessionClosed) {
  co_await setupMoQSession();

  EXPECT_CALL(*serverPublisher, subscribeNamespace(_, _))
      .WillOnce(
          testing::Invoke(
              [](auto /*subAnn*/, auto /*handler*/)
                  -> folly::coro::Task<Publisher::SubscribeNamespaceResult> {
                co_return std::make_shared<MockSubscribeNamespaceHandle>(
                    SubscribeNamespaceOk(
                        {.requestID = RequestID(0),
                         .requestSpecificParams = {}}));
              }));

  EXPECT_CALL(*clientSubscriberStatsCallback_, onSubscribeNamespaceSuccess());
  EXPECT_CALL(*serverPublisherStatsCallback_, onSubscribeNamespaceSuccess());
  auto subscribeNamespaceResult = co_await clientSession_->subscribeNamespace(
      getSubscribeNamespace(), nullptr);
  EXPECT_FALSE(subscribeNamespaceResult.hasError());

  // Close the session first, then unsubscribe - should not crash
  clientSession_->close(SessionCloseErrorCode::NO_ERROR);
  EXPECT_NO_THROW(subscribeNamespaceResult.value()->unsubscribeNamespace());
}

using V16PlusSubscribeNamespaceTest = MoQSessionTest;

class RelayHopNamespacePublishHandle
    : public Publisher::NamespacePublishHandle {
 public:
  void namespaceMsg(const Namespace& ns) override {
    message = ns;
    namespaceBaton.post();
  }

  void namespaceMsg(const TrackNamespace&) override {}
  void namespaceDoneMsg(const TrackNamespace&) override {
    namespaceDoneBaton.post();
  }

  std::optional<Namespace> message;
  folly::coro::Baton namespaceBaton;
  folly::coro::Baton namespaceDoneBaton;
};

// Verifies that after NAMESPACE + NAMESPACE_DONE, a second NAMESPACE
// can still be sent on the same stream.
folly::coro::Task<void> verifyNamespaceDoneDoesNotCloseStream(
    std::shared_ptr<Publisher::NamespacePublishHandle>& serverPublishHandle,
    std::shared_ptr<MockNamespacePublishHandle>& clientNamespacePublishHandle) {
  TrackNamespace ns1{{"bar"}};
  TrackNamespace ns2{{"baz"}};

  folly::coro::Baton namespaceBaton;
  folly::coro::Baton namespaceDoneBaton;
  folly::coro::Baton namespace2Baton;

  testing::InSequence seq;
  EXPECT_CALL(*clientNamespacePublishHandle, namespaceMsg(ns1))
      .WillOnce(testing::Invoke([&namespaceBaton](const TrackNamespace&) {
        namespaceBaton.post();
      }));
  EXPECT_CALL(*clientNamespacePublishHandle, namespaceDoneMsg(ns1))
      .WillOnce(testing::Invoke([&namespaceDoneBaton](const TrackNamespace&) {
        namespaceDoneBaton.post();
      }));
  EXPECT_CALL(*clientNamespacePublishHandle, namespaceMsg(ns2))
      .WillOnce(testing::Invoke([&namespace2Baton](const TrackNamespace&) {
        namespace2Baton.post();
      }));

  serverPublishHandle->namespaceMsg(ns1);
  co_await namespaceBaton;

  serverPublishHandle->namespaceDoneMsg(ns1);
  co_await namespaceDoneBaton;

  serverPublishHandle->namespaceMsg(ns2);
  co_await namespace2Baton;
}

CO_TEST_P_X(V16PlusSubscribeNamespaceTest, NamespaceDoneDoesNotCloseStream) {
  co_await setupMoQSession();

  std::shared_ptr<MockSubscribeNamespaceHandle> mockSubscribeNamespaceHandle;
  std::shared_ptr<Publisher::NamespacePublishHandle> serverPublishHandle;
  EXPECT_CALL(*serverPublisher, subscribeNamespace(_, _))
      .WillOnce(
          testing::Invoke(
              [&mockSubscribeNamespaceHandle, &serverPublishHandle](
                  auto subAnn, auto handler)
                  -> folly::coro::Task<Publisher::SubscribeNamespaceResult> {
                serverPublishHandle = handler;
                mockSubscribeNamespaceHandle =
                    std::make_shared<MockSubscribeNamespaceHandle>(
                        SubscribeNamespaceOk(
                            {.requestID = RequestID(0),
                             .requestSpecificParams = {}}));
                co_return mockSubscribeNamespaceHandle;
              }));

  auto clientNamespacePublishHandle =
      std::make_shared<MockNamespacePublishHandle>();

  EXPECT_CALL(*clientSubscriberStatsCallback_, onSubscribeNamespaceSuccess());
  EXPECT_CALL(*serverPublisherStatsCallback_, onSubscribeNamespaceSuccess());
  auto publishNamespaceResult = co_await clientSession_->subscribeNamespace(
      getSubscribeNamespace(), clientNamespacePublishHandle);
  EXPECT_FALSE(publishNamespaceResult.hasError());

  // Server sends NAMESPACE, then NAMESPACE_DONE, then another NAMESPACE.
  // Before the fix, NAMESPACE_DONE sent fin on the stream, so the second
  // NAMESPACE would not be received.
  co_await verifyNamespaceDoneDoesNotCloseStream(
      serverPublishHandle, clientNamespacePublishHandle);

  EXPECT_CALL(*clientSubscriberStatsCallback_, onUnsubscribeNamespace());
  EXPECT_CALL(*serverPublisherStatsCallback_, onUnsubscribeNamespace());

  folly::coro::Baton unsubBaton;
  EXPECT_CALL(*mockSubscribeNamespaceHandle, unsubscribeNamespace())
      .WillOnce(testing::Invoke([&unsubBaton]() { unsubBaton.post(); }));
  publishNamespaceResult.value()->unsubscribeNamespace();
  co_await unsubBaton;
  clientSession_->close(SessionCloseErrorCode::NO_ERROR);
}

CO_TEST_P_X(
    V16PlusSubscribeNamespaceTest,
    NamespacePreservesRelayHopParameters) {
  if (getDraftMajorVersion(GetParam().serverVersion) < 18) {
    co_return;
  }
  relayHopsSupported_ = true;
  co_await setupMoQSession();
  EXPECT_TRUE(
      clientSession_->negotiatedSetupExtension(SetupExtension::RelayHops));
  EXPECT_TRUE(
      serverSession_->negotiatedSetupExtension(SetupExtension::RelayHops));

  std::shared_ptr<MockSubscribeNamespaceHandle> serverSubscribeHandle;
  std::shared_ptr<Publisher::NamespacePublishHandle> serverPublishHandle;
  EXPECT_CALL(*serverPublisher, subscribeNamespace(_, _))
      .WillOnce(testing::Invoke(
          [&serverSubscribeHandle, &serverPublishHandle](
              auto subNs, auto handler)
              -> folly::coro::Task<Publisher::SubscribeNamespaceResult> {
            serverPublishHandle = std::move(handler);
            serverSubscribeHandle =
                std::make_shared<MockSubscribeNamespaceHandle>(
                    SubscribeNamespaceOk{
                        .requestID = subNs.requestID,
                        .requestSpecificParams = {},
                    });
            co_return serverSubscribeHandle;
          }));

  auto clientNamespacePublishHandle =
      std::make_shared<RelayHopNamespacePublishHandle>();
  EXPECT_CALL(*clientSubscriberStatsCallback_, onSubscribeNamespaceSuccess());
  EXPECT_CALL(*serverPublisherStatsCallback_, onSubscribeNamespaceSuccess());
  auto result = co_await clientSession_->subscribeNamespace(
      getSubscribeNamespace(), clientNamespacePublishHandle);
  EXPECT_TRUE(result.hasValue());
  if (!result.hasValue()) {
    co_return;
  }

  Namespace outgoing;
  outgoing.trackNamespaceSuffix = TrackNamespace{{"relay", "hops"}};
  auto encodedPath = encodeRelayHopPath({11, 22, 33}, GetParam().serverVersion);
  EXPECT_TRUE(encodedPath.hasValue());
  if (!encodedPath.hasValue()) {
    co_return;
  }
  outgoing.params.insertParam(Parameter(
      folly::to_underlying(TrackRequestParamKey::HOP_PATH),
      std::move(encodedPath.value())));

  serverPublishHandle->namespaceMsg(outgoing);
  co_await clientNamespacePublishHandle->namespaceBaton;
  EXPECT_TRUE(clientNamespacePublishHandle->message.has_value());
  if (!clientNamespacePublishHandle->message) {
    co_return;
  }
  const auto& incoming = *clientNamespacePublishHandle->message;
  EXPECT_EQ(incoming.trackNamespaceSuffix, outgoing.trackNamespaceSuffix);
  EXPECT_EQ(incoming.params.size(), 1);
  if (!incoming.params.empty()) {
    auto hopPath = decodeRelayHopPath(
        incoming.params.at(0).asString, GetParam().serverVersion);
    EXPECT_TRUE(hopPath.hasValue());
    if (hopPath.hasValue()) {
      EXPECT_EQ(hopPath.value(), (std::vector<uint64_t>{11, 22, 33}));
    }
  }

  clientNamespacePublishHandle->namespaceBaton.reset();
  outgoing.params.insertParam(Parameter(
      folly::to_underlying(TrackRequestParamKey::ROUTE_COST), uint64_t{8}));
  serverPublishHandle->namespaceMsg(outgoing);
  co_await clientNamespacePublishHandle->namespaceBaton;
  EXPECT_EQ(
      clientNamespacePublishHandle->message->params
          .getFirstParam(TrackRequestParamKey::ROUTE_COST)
          ->asUint64,
      8);
  EXPECT_FALSE(clientSession_->isClosed());

  EXPECT_CALL(*clientSubscriberStatsCallback_, onUnsubscribeNamespace());
  EXPECT_CALL(*serverPublisherStatsCallback_, onUnsubscribeNamespace());
  EXPECT_CALL(*serverSubscribeHandle, unsubscribeNamespace());
  result.value()->unsubscribeNamespace();
  clientSession_->close(SessionCloseErrorCode::NO_ERROR);
}

CO_TEST_P_X(
    V16PlusSubscribeNamespaceTest,
    NamespaceDoneBeforeOkDoesNotCloseStream) {
  co_await setupMoQSession();

  std::shared_ptr<MockSubscribeNamespaceHandle> mockSubscribeNamespaceHandle;
  std::shared_ptr<Publisher::NamespacePublishHandle> serverPublishHandle;
  EXPECT_CALL(*serverPublisher, subscribeNamespace(_, _))
      .WillOnce(
          testing::Invoke(
              [&mockSubscribeNamespaceHandle, &serverPublishHandle](
                  auto subAnn, auto handler)
                  -> folly::coro::Task<Publisher::SubscribeNamespaceResult> {
                serverPublishHandle = handler;
                // Send NAMESPACE + NAMESPACE_DONE before returning OK.
                // These get buffered and flushed when OK is sent.
                TrackNamespace ns1{{"bar"}};
                handler->namespaceMsg(ns1);
                handler->namespaceDoneMsg(ns1);
                mockSubscribeNamespaceHandle =
                    std::make_shared<MockSubscribeNamespaceHandle>(
                        SubscribeNamespaceOk(
                            {.requestID = RequestID(0),
                             .requestSpecificParams = {}}));
                co_return mockSubscribeNamespaceHandle;
              }));

  auto clientNamespacePublishHandle =
      std::make_shared<MockNamespacePublishHandle>();

  // The first NAMESPACE + NAMESPACE_DONE were sent before OK, so set up
  // expectations before the subscribeNamespace call triggers OK + flush.
  TrackNamespace ns1{{"bar"}};
  TrackNamespace ns2{{"baz"}};

  folly::coro::Baton namespaceBaton;
  folly::coro::Baton namespaceDoneBaton;

  {
    testing::InSequence seq;
    EXPECT_CALL(*clientNamespacePublishHandle, namespaceMsg(ns1))
        .WillOnce(testing::Invoke([&namespaceBaton](const TrackNamespace&) {
          namespaceBaton.post();
        }));
    EXPECT_CALL(*clientNamespacePublishHandle, namespaceDoneMsg(ns1))
        .WillOnce(testing::Invoke([&namespaceDoneBaton](const TrackNamespace&) {
          namespaceDoneBaton.post();
        }));
  }

  EXPECT_CALL(*clientSubscriberStatsCallback_, onSubscribeNamespaceSuccess());
  EXPECT_CALL(*serverPublisherStatsCallback_, onSubscribeNamespaceSuccess());
  auto publishNamespaceResult = co_await clientSession_->subscribeNamespace(
      getSubscribeNamespace(), clientNamespacePublishHandle);
  EXPECT_FALSE(publishNamespaceResult.hasError());

  co_await namespaceBaton;
  co_await namespaceDoneBaton;

  // Send another NAMESPACE after the buffered NAMESPACE_DONE was flushed.
  // Before the fix, pendingFin_ was true, so flushPendingMessages sent fin
  // and this second message would not be received.
  folly::coro::Baton namespace2Baton;
  EXPECT_CALL(*clientNamespacePublishHandle, namespaceMsg(ns2))
      .WillOnce(testing::Invoke([&namespace2Baton](const TrackNamespace&) {
        namespace2Baton.post();
      }));

  serverPublishHandle->namespaceMsg(ns2);
  co_await namespace2Baton;

  EXPECT_CALL(*clientSubscriberStatsCallback_, onUnsubscribeNamespace());
  EXPECT_CALL(*serverPublisherStatsCallback_, onUnsubscribeNamespace());

  folly::coro::Baton unsubBaton;
  EXPECT_CALL(*mockSubscribeNamespaceHandle, unsubscribeNamespace())
      .WillOnce(testing::Invoke([&unsubBaton]() { unsubBaton.post(); }));
  publishNamespaceResult.value()->unsubscribeNamespace();
  co_await unsubBaton;
  clientSession_->close(SessionCloseErrorCode::NO_ERROR);
}

INSTANTIATE_TEST_SUITE_P(
    V16PlusSubscribeNamespaceTest,
    V16PlusSubscribeNamespaceTest,
    testing::Values(
        VersionParams{{kVersionDraft16}, kVersionDraft16},
        VersionParams{{kVersionDraft18}, kVersionDraft18}));

CO_TEST_P_X(MoQSessionTest, SubscribeNamespaceError) {
  co_await setupMoQSession();

  EXPECT_CALL(*serverPublisher, subscribeNamespace(_, _))
      .WillOnce(
          testing::Invoke(
              [](auto subAnn, auto /*handler*/)
                  -> folly::coro::Task<Publisher::SubscribeNamespaceResult> {
                SubscribeNamespaceError subAnnError{
                    subAnn.requestID,
                    SubscribeNamespaceErrorCode::NOT_SUPPORTED,
                    "not supported"};
                co_return folly::makeUnexpected(subAnnError);
              }));

  EXPECT_CALL(
      *clientSubscriberStatsCallback_,
      onSubscribeNamespaceError(SubscribeNamespaceErrorCode::NOT_SUPPORTED));
  EXPECT_CALL(
      *serverPublisherStatsCallback_,
      onSubscribeNamespaceError(SubscribeNamespaceErrorCode::NOT_SUPPORTED));
  auto subAnnResult = co_await clientSession_->subscribeNamespace(
      getSubscribeNamespace(), nullptr);
  EXPECT_TRUE(subAnnResult.hasError());

  clientSession_->close(SessionCloseErrorCode::NO_ERROR);
}

CO_TEST_P_X(V16PlusSubscribeNamespaceTest, ClusterNamespaceStreamOwnership) {
  if (getDraftMajorVersion(GetParam().serverVersion) < 18) {
    co_return;
  }
  relayHopsSupported_ = true;
  co_await setupMoQSession();
  std::vector<std::shared_ptr<Publisher::NamespacePublishHandle>> senders;
  auto accept = [&](auto request, auto handler)
      -> folly::coro::Task<Publisher::SubscribeNamespaceResult> {
    senders.push_back(std::move(handler));
    auto handle =
        std::make_shared<testing::NiceMock<MockSubscribeNamespaceHandle>>(
            SubscribeNamespaceOk{.requestID = request.requestID});
    ON_CALL(*handle, requestUpdateResult())
        .WillByDefault(testing::Return(RequestOk{}));
    co_return handle;
  };
  EXPECT_CALL(*serverPublisher, subscribeNamespace(_, _))
      .WillRepeatedly(testing::Invoke(std::ref(accept)));
  EXPECT_CALL(*clientSubscriberStatsCallback_, onSubscribeNamespaceSuccess())
      .Times(2);
  EXPECT_CALL(*serverPublisherStatsCallback_, onSubscribeNamespaceSuccess())
      .Times(2);
  auto firstReceiver = std::make_shared<RelayHopNamespacePublishHandle>();
  auto secondReceiver = std::make_shared<RelayHopNamespacePublishHandle>();
  auto request = getSubscribeNamespace();
  request.trackNamespacePrefix = TrackNamespace({"cluster"});
  auto first =
      co_await clientSession_->subscribeNamespace(request, firstReceiver);
  request.trackNamespacePrefix =
      TrackNamespace(std::vector<std::string>{"cluster", "nested"});
  auto second =
      co_await clientSession_->subscribeNamespace(request, secondReceiver);
  EXPECT_TRUE(first.hasValue());
  EXPECT_TRUE(second.hasValue());
  if (!first || !second) {
    co_return;
  }
  Namespace outer;
  outer.trackNamespaceSuffix =
      TrackNamespace(std::vector<std::string>{"nested", "leaf"});
  outer.params.insertParam(Parameter(
      folly::to_underlying(TrackRequestParamKey::HOP_PATH),
      std::string("\x01", 1)));
  Namespace inner;
  inner.trackNamespaceSuffix = TrackNamespace({"leaf"});
  inner.params.insertParam(outer.params.at(0));
  senders[0]->namespaceMsg(outer);
  co_await firstReceiver->namespaceBaton;
  senders[1]->namespaceDoneMsg(inner.trackNamespaceSuffix);
  senders[1]->namespaceMsg(inner);
  for (int i = 0; i < 25; ++i) {
    co_await folly::coro::co_reschedule_on_current_executor;
  }
  EXPECT_FALSE(secondReceiver->message.has_value());
  EXPECT_FALSE(clientSession_->isClosed());

  PublishNamespace conflicting;
  conflicting.trackNamespace = TrackNamespace({"cluster", "nested", "leaf"});
  conflicting.params.insertParam(outer.params.at(0));
  auto duplicate =
      co_await serverSession_->publishNamespace(conflicting, nullptr);
  EXPECT_TRUE(duplicate.hasError());

  senders[0]->namespaceDoneMsg(outer.trackNamespaceSuffix);
  senders[1]->namespaceMsg(inner);
  co_await secondReceiver->namespaceBaton;
  EXPECT_TRUE(secondReceiver->message.has_value());
  EXPECT_FALSE(clientSession_->isClosed());

  EXPECT_CALL(*clientSubscriberStatsCallback_, onUnsubscribeNamespace())
      .Times(1);
  EXPECT_CALL(*serverPublisherStatsCallback_, onUnsubscribeNamespace())
      .Times(2);
  second.value()->unsubscribeNamespace();
  for (int i = 0; i < 25; ++i) {
    co_await folly::coro::co_reschedule_on_current_executor;
  }
  firstReceiver->namespaceBaton.reset();
  senders[0]->namespaceMsg(outer);
  co_await firstReceiver->namespaceBaton;
  EXPECT_FALSE(clientSession_->isClosed());
  EXPECT_CALL(*clientSubscriberStatsCallback_, onRequestUpdate());
  RequestUpdate update;
  update.params.setMajorVersion(18);
  update.params.insertParam(MoQFrameWriter::encodeTrackNamespacePrefixParam(
      TrackNamespace(std::vector<std::string>{"cluster", "nested"}),
      GetParam().serverVersion));
  auto updated = co_await first.value()->requestUpdate(std::move(update));
  EXPECT_TRUE(updated.hasValue());
  firstReceiver->namespaceDoneBaton.reset();
  senders[0]->namespaceDoneMsg(inner.trackNamespaceSuffix);
  co_await firstReceiver->namespaceDoneBaton;
  EXPECT_FALSE(clientSession_->isClosed());
  clientSession_->close(SessionCloseErrorCode::NO_ERROR);
}

CO_TEST_P_X(
    V16PlusSubscribeNamespaceTest,
    ClusterRejectsNamespaceOnSecondResponseStream) {
  if (getDraftMajorVersion(GetParam().serverVersion) < 18) {
    co_return;
  }
  relayHopsSupported_ = true;
  co_await setupMoQSession();
  EXPECT_CALL(*serverPublisher, subscribeNamespace(_, _))
      .WillRepeatedly(testing::Invoke(
          [](auto request,
             auto) -> folly::coro::Task<Publisher::SubscribeNamespaceResult> {
            co_return std::make_shared<
                testing::NiceMock<MockSubscribeNamespaceHandle>>(
                SubscribeNamespaceOk{.requestID = request.requestID});
          }));
  EXPECT_CALL(*clientSubscriberStatsCallback_, onSubscribeNamespaceSuccess())
      .Times(2);
  EXPECT_CALL(*serverPublisherStatsCallback_, onSubscribeNamespaceSuccess())
      .Times(2);
  auto receiver = std::make_shared<RelayHopNamespacePublishHandle>();
  auto request = getSubscribeNamespace();
  request.trackNamespacePrefix = TrackNamespace({"cluster"});
  auto first = co_await clientSession_->subscribeNamespace(request, receiver);
  uint64_t firstStream = 0;
  for (const auto& [id, handle] : serverWt_->writeHandles) {
    if (id % 4 == 0) {
      firstStream = std::max(firstStream, id);
    }
  }
  request.trackNamespacePrefix =
      TrackNamespace(std::vector<std::string>{"cluster", "nested"});
  auto second = co_await clientSession_->subscribeNamespace(request, receiver);
  uint64_t secondStream = firstStream;
  for (const auto& [id, handle] : serverWt_->writeHandles) {
    if (id % 4 == 0) {
      secondStream = std::max(secondStream, id);
    }
  }
  EXPECT_NE(firstStream, secondStream);
  Namespace ns;
  ns.trackNamespaceSuffix =
      TrackNamespace(std::vector<std::string>{"nested", "leaf"});
  ns.params.insertParam(Parameter(
      folly::to_underlying(TrackRequestParamKey::HOP_PATH),
      std::string("\x01", 1)));
  MoQFrameWriter writer;
  writer.initializeVersion(
      GetParam().serverVersion, serverSession_->getNegotiatedExtensions());
  folly::IOBufQueue buf{folly::IOBufQueue::cacheChainLength()};
  EXPECT_TRUE(writer.writeNamespace(buf, ns).hasValue());
  serverWt_->writeHandles.at(firstStream)
      ->writeStreamData(buf.move(), false, nullptr);
  co_await receiver->namespaceBaton;
  ns.trackNamespaceSuffix = TrackNamespace({"leaf"});
  EXPECT_TRUE(writer.writeNamespace(buf, ns).hasValue());
  serverWt_->writeHandles.at(secondStream)
      ->writeStreamData(buf.move(), false, nullptr);
  for (int i = 0; i < 50; ++i) {
    co_await folly::coro::co_reschedule_on_current_executor;
  }
  EXPECT_TRUE(clientSession_->isClosed());
  clientSession_->close(SessionCloseErrorCode::NO_ERROR);
}

CO_TEST_P_X(
    V16PlusSubscribeNamespaceTest,
    ClusterRejectsPublishNamespaceClaimedByResponseStream) {
  if (getDraftMajorVersion(GetParam().serverVersion) < 18) {
    co_return;
  }
  relayHopsSupported_ = true;
  co_await setupMoQSession();
  EXPECT_CALL(*serverPublisher, subscribeNamespace(_, _))
      .WillRepeatedly(testing::Invoke(
          [](auto request,
             auto) -> folly::coro::Task<Publisher::SubscribeNamespaceResult> {
            co_return std::make_shared<
                testing::NiceMock<MockSubscribeNamespaceHandle>>(
                SubscribeNamespaceOk{.requestID = request.requestID});
          }));
  EXPECT_CALL(*clientSubscriberStatsCallback_, onSubscribeNamespaceSuccess())
      .Times(1);
  EXPECT_CALL(*serverPublisherStatsCallback_, onSubscribeNamespaceSuccess())
      .Times(1);
  auto receiver = std::make_shared<RelayHopNamespacePublishHandle>();
  auto request = getSubscribeNamespace();
  request.trackNamespacePrefix = TrackNamespace({"cluster"});
  auto first = co_await clientSession_->subscribeNamespace(request, receiver);
  uint64_t firstStream = 0;
  for (const auto& [id, handle] : serverWt_->writeHandles) {
    if (id % 4 == 0) {
      firstStream = std::max(firstStream, id);
    }
  }
  Namespace ns;
  ns.trackNamespaceSuffix =
      TrackNamespace(std::vector<std::string>{"nested", "leaf"});
  ns.params.insertParam(Parameter(
      folly::to_underlying(TrackRequestParamKey::HOP_PATH),
      std::string("\x01", 1)));
  MoQFrameWriter writer;
  writer.initializeVersion(
      GetParam().serverVersion, serverSession_->getNegotiatedExtensions());
  folly::IOBufQueue buf{folly::IOBufQueue::cacheChainLength()};
  EXPECT_TRUE(writer.writeNamespace(buf, ns).hasValue());
  serverWt_->writeHandles.at(firstStream)
      ->writeStreamData(buf.move(), false, nullptr);
  co_await receiver->namespaceBaton;
  PublishNamespace ann;
  ann.requestID = RequestID(1);
  ann.trackNamespace = TrackNamespace({"cluster", "nested", "leaf"});
  ann.params.insertParam(ns.params.at(0));
  auto stream = serverWt_->createBidiStream();
  EXPECT_TRUE(stream.hasValue());
  if (!stream) {
    co_return;
  }
  EXPECT_TRUE(writer.writePublishNamespace(buf, ann).hasValue());
  stream->writeHandle->writeStreamData(buf.move(), false, nullptr);
  for (int i = 0; i < 50; ++i) {
    co_await folly::coro::co_reschedule_on_current_executor;
  }
  EXPECT_TRUE(clientSession_->isClosed());
  clientSession_->close(SessionCloseErrorCode::NO_ERROR);
}

CO_TEST_P_X(
    V16PlusSubscribeNamespaceTest,
    ClusterNamespaceOwnerReleasedOnPeerFin) {
  if (getDraftMajorVersion(GetParam().serverVersion) < 18) {
    co_return;
  }
  relayHopsSupported_ = true;
  co_await setupMoQSession();
  EXPECT_CALL(*serverPublisher, subscribeNamespace(_, _))
      .WillRepeatedly(testing::Invoke(
          [](auto request,
             auto) -> folly::coro::Task<Publisher::SubscribeNamespaceResult> {
            co_return std::make_shared<
                testing::NiceMock<MockSubscribeNamespaceHandle>>(
                SubscribeNamespaceOk{.requestID = request.requestID});
          }));
  EXPECT_CALL(*clientSubscriberStatsCallback_, onSubscribeNamespaceSuccess())
      .Times(2);
  EXPECT_CALL(*serverPublisherStatsCallback_, onSubscribeNamespaceSuccess())
      .Times(2);
  auto receiver = std::make_shared<RelayHopNamespacePublishHandle>();
  auto request = getSubscribeNamespace();
  request.trackNamespacePrefix = TrackNamespace({"cluster"});
  auto first = co_await clientSession_->subscribeNamespace(request, receiver);
  uint64_t firstStream = 0;
  for (const auto& [id, handle] : serverWt_->writeHandles) {
    if (id % 4 == 0) {
      firstStream = std::max(firstStream, id);
    }
  }
  request.trackNamespacePrefix =
      TrackNamespace(std::vector<std::string>{"cluster", "nested"});
  auto second = co_await clientSession_->subscribeNamespace(request, receiver);
  uint64_t secondStream = firstStream;
  for (const auto& [id, handle] : serverWt_->writeHandles) {
    if (id % 4 == 0) {
      secondStream = std::max(secondStream, id);
    }
  }
  EXPECT_NE(firstStream, secondStream);
  Namespace ns;
  ns.trackNamespaceSuffix =
      TrackNamespace(std::vector<std::string>{"nested", "leaf"});
  ns.params.insertParam(Parameter(
      folly::to_underlying(TrackRequestParamKey::HOP_PATH),
      std::string("\x01", 1)));
  MoQFrameWriter writer;
  writer.initializeVersion(
      GetParam().serverVersion, serverSession_->getNegotiatedExtensions());
  folly::IOBufQueue buf{folly::IOBufQueue::cacheChainLength()};
  EXPECT_TRUE(writer.writeNamespace(buf, ns).hasValue());
  serverWt_->writeHandles.at(firstStream)
      ->writeStreamData(buf.move(), false, nullptr);
  co_await receiver->namespaceBaton;
  serverWt_->writeHandles.at(firstStream)
      ->writeStreamData(nullptr, true, nullptr);
  for (int i = 0; i < 25; ++i) {
    co_await folly::coro::co_reschedule_on_current_executor;
  }
  receiver->namespaceBaton.reset();
  ns.trackNamespaceSuffix = TrackNamespace({"leaf"});
  EXPECT_TRUE(writer.writeNamespace(buf, ns).hasValue());
  serverWt_->writeHandles.at(secondStream)
      ->writeStreamData(buf.move(), false, nullptr);
  for (int i = 0; i < 50; ++i) {
    co_await folly::coro::co_reschedule_on_current_executor;
  }
  co_await receiver->namespaceBaton;
  EXPECT_FALSE(clientSession_->isClosed());
  clientSession_->close(SessionCloseErrorCode::NO_ERROR);
}

CO_TEST_P_X(
    V16PlusSubscribeNamespaceTest,
    ClusterRejectsNamespaceDoneOnAnotherResponseStream) {
  if (getDraftMajorVersion(GetParam().serverVersion) < 18) {
    co_return;
  }
  relayHopsSupported_ = true;
  co_await setupMoQSession();
  EXPECT_CALL(*serverPublisher, subscribeNamespace(_, _))
      .WillRepeatedly(testing::Invoke(
          [](auto request,
             auto) -> folly::coro::Task<Publisher::SubscribeNamespaceResult> {
            co_return std::make_shared<
                testing::NiceMock<MockSubscribeNamespaceHandle>>(
                SubscribeNamespaceOk{.requestID = request.requestID});
          }));
  EXPECT_CALL(*clientSubscriberStatsCallback_, onSubscribeNamespaceSuccess())
      .Times(2);
  EXPECT_CALL(*serverPublisherStatsCallback_, onSubscribeNamespaceSuccess())
      .Times(2);
  auto receiver = std::make_shared<RelayHopNamespacePublishHandle>();
  auto request = getSubscribeNamespace();
  request.trackNamespacePrefix = TrackNamespace({"cluster"});
  auto first = co_await clientSession_->subscribeNamespace(request, receiver);
  uint64_t firstStream = 0;
  for (const auto& [id, handle] : serverWt_->writeHandles) {
    if (id % 4 == 0) {
      firstStream = std::max(firstStream, id);
    }
  }
  request.trackNamespacePrefix =
      TrackNamespace(std::vector<std::string>{"cluster", "nested"});
  auto second = co_await clientSession_->subscribeNamespace(request, receiver);
  uint64_t secondStream = firstStream;
  for (const auto& [id, handle] : serverWt_->writeHandles) {
    if (id % 4 == 0) {
      secondStream = std::max(secondStream, id);
    }
  }
  EXPECT_NE(firstStream, secondStream);
  Namespace ns;
  ns.trackNamespaceSuffix =
      TrackNamespace(std::vector<std::string>{"nested", "leaf"});
  ns.params.insertParam(Parameter(
      folly::to_underlying(TrackRequestParamKey::HOP_PATH),
      std::string("\x01", 1)));
  MoQFrameWriter writer;
  writer.initializeVersion(
      GetParam().serverVersion, serverSession_->getNegotiatedExtensions());
  folly::IOBufQueue buf{folly::IOBufQueue::cacheChainLength()};
  EXPECT_TRUE(writer.writeNamespace(buf, ns).hasValue());
  serverWt_->writeHandles.at(firstStream)
      ->writeStreamData(buf.move(), false, nullptr);
  co_await receiver->namespaceBaton;
  NamespaceDone done{TrackNamespace({"leaf"})};
  EXPECT_TRUE(writer.writeNamespaceDone(buf, done).hasValue());
  serverWt_->writeHandles.at(secondStream)
      ->writeStreamData(buf.move(), false, nullptr);
  for (int i = 0; i < 50; ++i) {
    co_await folly::coro::co_reschedule_on_current_executor;
  }
  EXPECT_TRUE(clientSession_->isClosed());
  clientSession_->close(SessionCloseErrorCode::NO_ERROR);
}
