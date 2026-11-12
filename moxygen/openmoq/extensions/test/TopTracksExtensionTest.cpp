/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <moxygen/openmoq/extensions/TopTracksExtension.h>

#include <moxygen/test/MoQSessionTestCommon.h>

using namespace moxygen;
using namespace moxygen::test;
using testing::_;

namespace {

using TopTracks = TopTracksExtension;

SetupParameters declaring(std::optional<uint64_t> maxTopTracks) {
  SetupParameters params(FrameType::CLIENT_SETUP);
  if (maxTopTracks) {
    EXPECT_TRUE(params
                    .insertParam(SetupParameter(
                        TopTracks::kMaxTopTracksOption, *maxTopTracks))
                    .hasValue());
  }
  return params;
}

} // namespace

TEST(TopTracksFilterCodecTest, RoundTripsOnEveryVarintForm) {
  for (auto version : {kVersionDraft16, kVersionDraft18}) {
    for (const auto& filter :
         {TrackFilter{2, 3},
          TrackFilter{0, 1},
          TrackFilter{quic::kEightByteLimit - 1, quic::kEightByteLimit}}) {
      SCOPED_TRACE(testing::Message() << version << " " << filter.maxSelected);
      auto param = TopTracks::encode(filter, version);
      EXPECT_EQ(param.key, TopTracks::kTopTracksFilterParam);
      EXPECT_EQ(TopTracks::decode(param, version), filter);
    }
  }
}

TEST(TopTracksFilterCodecTest, ClipsPastTheVarintRange) {
  const TrackFilter huge{2, std::numeric_limits<uint64_t>::max()};
  EXPECT_EQ(
      TopTracks::decode(
          TopTracks::encode(huge, kVersionDraft16), kVersionDraft16),
      (TrackFilter{2, quic::kEightByteLimit}));
  EXPECT_EQ(
      TopTracks::decode(
          TopTracks::encode(huge, kVersionDraft18), kVersionDraft18),
      huge);
}

TEST(TopTracksFilterCodecTest, RejectsShortTrailingAndEmptyValues) {
  auto param = TopTracks::encode(TrackFilter{2, 3}, kVersionDraft18);
  auto trailing = param;
  trailing.asString.push_back('\0');
  EXPECT_FALSE(TopTracks::decode(trailing, kVersionDraft18));
  auto shortValue = param;
  shortValue.asString.pop_back();
  EXPECT_FALSE(TopTracks::decode(shortValue, kVersionDraft18));
  EXPECT_FALSE(TopTracks::decode(TopTracks::encodeRemoval(), kVersionDraft18));
}

TEST(TopTracksFilterCodecTest, EncodesInTheNegotiatedVarint) {
  EXPECT_NE(
      TopTracks::encode(TrackFilter{64, 1}, kVersionDraft16).asString,
      TopTracks::encode(TrackFilter{64, 1}, kVersionDraft18).asString);
}

TEST(TopTracksExtensionTest, DeclaresANonzeroLimit) {
  for (auto version : {kVersionDraft16, kVersionDraft18}) {
    SCOPED_TRACE(version);
    EXPECT_TRUE(TopTracks(0).getSetupOptions(version).empty());
    auto options = TopTracks(5).getSetupOptions(version);
    ASSERT_EQ(options.size(), 1);
    EXPECT_EQ(options[0].key, TopTracks::kMaxTopTracksOption);
    EXPECT_EQ(options[0].asUint64, 5);
  }
}

TEST(TopTracksExtensionTest, OffWhenNeitherSideDeclares) {
  EXPECT_TRUE(
      TopTracks(0)
          .negotiate(kVersionDraft18, declaring(std::nullopt), declaring(0))
          ->empty());
}

// Runs on every draft moxygen speaks: the filter is odd, so it is
// length-prefixed even where parity sets the encoding. Draft 18 and later
// carry it on SUBSCRIBE_TRACKS; earlier drafts on SUBSCRIBE_NAMESPACE.
class TopTracksFramerTest : public testing::TestWithParam<uint64_t> {
 protected:
  bool tracksFrame() const {
    return getDraftMajorVersion(GetParam()) >= 18;
  }

  std::shared_ptr<const MessageParameterExtensions> rules(
      uint64_t localLimit,
      uint64_t peerLimit) {
    auto negotiated =
        TopTracks(localLimit)
            .negotiate(GetParam(), declaring(localLimit), declaring(peerLimit));
    EXPECT_TRUE(negotiated.hasValue());
    return std::make_shared<MessageParameterExtensions>(
        std::move(negotiated.value()));
  }

  // Lets any value onto the wire, so the parse side can be tested.
  std::shared_ptr<const MessageParameterExtensions> permissive() {
    auto loose = std::make_shared<MessageParameterExtensions>(*rules(5, 5));
    for (auto& [key, rule] : *loose) {
      rule.repeatable = true;
      rule.validate = nullptr;
    }
    return loose;
  }

  Parameter filter(uint64_t propertyType, uint64_t maxSelected) {
    return TopTracks::encode(
        TrackFilter{propertyType, maxSelected}, GetParam());
  }

  std::vector<Parameter> filterOf(uint64_t maxSelected) {
    return {filter(2, maxSelected)};
  }

  MoQFrameWriter writer(
      std::shared_ptr<const MessageParameterExtensions> extensions) {
    MoQFrameWriter writer;
    writer.initializeVersion(GetParam());
    writer.setMessageParameterExtensions(std::move(extensions));
    return writer;
  }

  MoQFrameParser parser(
      std::shared_ptr<const MessageParameterExtensions> extensions) {
    MoQFrameParser parser;
    parser.initializeVersion(GetParam());
    parser.setMessageParameterExtensions(std::move(extensions));
    return parser;
  }

  // Writes the draft's subscribe frame: SUBSCRIBE_TRACKS from draft 18,
  // SUBSCRIBE_NAMESPACE before.
  std::unique_ptr<folly::IOBuf> writeSubscribe(
      std::shared_ptr<const MessageParameterExtensions> extensions,
      std::vector<Parameter> params) {
    return tracksFrame()
        ? writeSubscribeTracks(std::move(extensions), std::move(params))
        : writeSubscribeNamespace(std::move(extensions), std::move(params));
  }

  folly::Expected<TrackRequestParameters, ErrorCode> parseSubscribe(
      std::shared_ptr<const MessageParameterExtensions> extensions,
      const folly::IOBuf& serialized) {
    auto framer = parser(std::move(extensions));
    folly::io::Cursor cursor(&serialized);
    EXPECT_TRUE(framer.decodeVarint(cursor).has_value());
    const auto length = cursor.readBE<uint16_t>();
    if (tracksFrame()) {
      auto parsed = framer.parseSubscribeTracks(cursor, length);
      if (parsed.hasError()) {
        return folly::makeUnexpected(parsed.error());
      }
      return std::move(parsed->params);
    }
    auto parsed = framer.parseSubscribeNamespace(cursor, length);
    if (parsed.hasError()) {
      return folly::makeUnexpected(parsed.error());
    }
    return std::move(parsed->params);
  }

  std::unique_ptr<folly::IOBuf> writeSubscribeNamespace(
      std::shared_ptr<const MessageParameterExtensions> extensions,
      std::vector<Parameter> params) {
    SubscribeNamespace subNs;
    subNs.requestID = RequestID(0);
    subNs.trackNamespacePrefix = TrackNamespace({"room"});
    for (auto& param : params) {
      EXPECT_TRUE(subNs.params.insertParam(std::move(param)).hasValue());
    }
    folly::IOBufQueue buf{folly::IOBufQueue::cacheChainLength()};
    if (writer(std::move(extensions))
            .writeSubscribeNamespace(buf, subNs)
            .hasError()) {
      return nullptr;
    }
    return buf.move();
  }

  std::unique_ptr<folly::IOBuf> writeSubscribeTracks(
      std::shared_ptr<const MessageParameterExtensions> extensions,
      std::vector<Parameter> params) {
    SubscribeTracks subTracks;
    subTracks.requestID = RequestID(0);
    subTracks.trackNamespacePrefix = TrackNamespace({"room"});
    for (auto& param : params) {
      EXPECT_TRUE(subTracks.params.insertParam(std::move(param)).hasValue());
    }
    folly::IOBufQueue buf{folly::IOBufQueue::cacheChainLength()};
    if (writer(std::move(extensions))
            .writeSubscribeTracks(buf, subTracks)
            .hasError()) {
      return nullptr;
    }
    return buf.move();
  }

  std::unique_ptr<folly::IOBuf> writeRequestUpdate(
      std::shared_ptr<const MessageParameterExtensions> extensions,
      std::vector<Parameter> params) {
    RequestUpdate update;
    update.requestID = RequestID(2);
    for (auto& param : params) {
      EXPECT_TRUE(update.params.insertParam(std::move(param)).hasValue());
    }
    folly::IOBufQueue buf{folly::IOBufQueue::cacheChainLength()};
    if (writer(std::move(extensions))
            .writeRequestUpdate(buf, update)
            .hasError()) {
      return nullptr;
    }
    return buf.move();
  }

  folly::Expected<RequestUpdate, ErrorCode> parseRequestUpdate(
      std::shared_ptr<const MessageParameterExtensions> extensions,
      const folly::IOBuf& serialized) {
    auto framer = parser(std::move(extensions));
    folly::io::Cursor cursor(&serialized);
    EXPECT_TRUE(framer.decodeVarint(cursor).has_value());
    return framer.parseRequestUpdate(cursor, cursor.readBE<uint16_t>());
  }
};

INSTANTIATE_TEST_SUITE_P(
    TopTracksFramerTest,
    TopTracksFramerTest,
    testing::Values(
        kVersionDraft16,
        kVersionDraft17,
        kVersionDraft18,
        kVersionDraft22));

TEST_P(TopTracksFramerTest, EachDirectionHasItsOwnLimit) {
  // We accept up to 2 and the peer accepts up to 5; `theirs` is the peer's
  // view of the same session.
  auto ours = rules(2, 5);
  auto theirs = rules(5, 2);

  auto sent = writeSubscribe(ours, filterOf(5));
  ASSERT_NE(sent, nullptr);
  auto parsed = parseSubscribe(theirs, *sent);
  ASSERT_TRUE(parsed.hasValue());
  EXPECT_EQ(TopTracks::find(*parsed, GetParam()), (TrackFilter{2, 5}));
  EXPECT_EQ(writeSubscribe(ours, filterOf(6)), nullptr);

  auto withinOurs = writeSubscribe(theirs, filterOf(2));
  ASSERT_NE(withinOurs, nullptr);
  EXPECT_TRUE(parseSubscribe(ours, *withinOurs).hasValue());
  EXPECT_EQ(writeSubscribe(theirs, filterOf(3)), nullptr);
}

TEST_P(TopTracksFramerTest, ZeroLimitForbidsTheParameter) {
  // We declared nothing, so the peer may not send a filter, even an empty one.
  auto ours = rules(0, 5);
  for (const auto& param : {filter(2, 1), TopTracks::encodeRemoval()}) {
    auto serialized = writeSubscribe(permissive(), {param});
    ASSERT_NE(serialized, nullptr);
    EXPECT_TRUE(parseSubscribe(ours, *serialized).hasError());
  }
  // The peer declared 5, so we may send one.
  EXPECT_NE(writeSubscribe(ours, {filter(2, 1)}), nullptr);
}

TEST_P(TopTracksFramerTest, InvalidFiltersAreRejectedBothWays) {
  // We accept up to 2.
  auto ours = rules(2, 5);
  auto malformed = filter(2, 1);
  malformed.asString.push_back('\0');
  const std::vector<std::pair<std::string, std::vector<Parameter>>> invalid{
      {"zero count", {filter(2, 0)}},
      {"odd property type", {filter(3, 1)}},
      {"above our limit", {filter(2, 3)}},
      {"malformed", {malformed}},
      {"repeated", {filter(2, 1), filter(2, 2)}},
  };
  for (const auto& [name, params] : invalid) {
    SCOPED_TRACE(name);
    EXPECT_EQ(writeSubscribe(rules(5, 2), params), nullptr);
    auto serialized = writeSubscribe(permissive(), params);
    ASSERT_NE(serialized, nullptr);
    auto parsed = parseSubscribe(ours, *serialized);
    ASSERT_TRUE(parsed.hasError());
    EXPECT_EQ(parsed.error(), ErrorCode::PROTOCOL_VIOLATION);
  }
}

TEST_P(TopTracksFramerTest, RidesOnlyOnTheDraftsSubscribeFrame) {
  if (!tracksFrame()) {
    GTEST_SKIP() << "SUBSCRIBE_NAMESPACE is the subscribe frame before 18";
  }
  EXPECT_EQ(writeSubscribeNamespace(rules(5, 5), filterOf(1)), nullptr);

  // A peer that puts the filter on SUBSCRIBE_NAMESPACE anyway.
  auto loose = std::make_shared<MessageParameterExtensions>(*permissive());
  for (auto& [key, rule] : *loose) {
    rule.allowedFrames.push_back(FrameType::SUBSCRIBE_NAMESPACE);
  }
  auto serialized = writeSubscribeNamespace(loose, filterOf(1));
  ASSERT_NE(serialized, nullptr);
  auto framer = parser(rules(5, 5));
  folly::io::Cursor cursor(serialized.get());
  EXPECT_TRUE(framer.decodeVarint(cursor).has_value());
  auto parsed =
      framer.parseSubscribeNamespace(cursor, cursor.readBE<uint16_t>());
  ASSERT_TRUE(parsed.hasError());
  EXPECT_EQ(parsed.error(), ErrorCode::PROTOCOL_VIOLATION);
}

TEST_P(TopTracksFramerTest, RequestUpdateCarriesARemovalWithinTheLimit) {
  auto ours = rules(2, 5);
  auto removal = writeRequestUpdate(ours, {TopTracks::encodeRemoval()});
  ASSERT_NE(removal, nullptr);
  auto parsed = parseRequestUpdate(rules(5, 2), *removal);
  ASSERT_TRUE(parsed.hasValue());
  EXPECT_TRUE(parsed->params.hasParam(TopTracks::kTopTracksFilterParam));
  EXPECT_FALSE(TopTracks::find(parsed->params, GetParam()));

  auto tooMany = writeRequestUpdate(permissive(), filterOf(3));
  ASSERT_NE(tooMany, nullptr);
  auto rejected = parseRequestUpdate(ours, *tooMany);
  ASSERT_TRUE(rejected.hasError());
  EXPECT_EQ(rejected.error(), ErrorCode::PROTOCOL_VIOLATION);
}

TEST_P(TopTracksFramerTest, UnnegotiatedFilterIsRejected) {
  EXPECT_EQ(writeSubscribe(nullptr, filterOf(1)), nullptr);
  auto serialized = writeSubscribe(rules(5, 5), filterOf(1));
  ASSERT_NE(serialized, nullptr);
  EXPECT_TRUE(parseSubscribe(nullptr, *serialized).hasError());
}

using TopTracksNamespaceSessionTest = MoQSessionTest;

INSTANTIATE_TEST_SUITE_P(
    TopTracksNamespaceSessionTest,
    TopTracksNamespaceSessionTest,
    testing::Values(VersionParams{{kVersionDraft16}, kVersionDraft16}));

CO_TEST_P_X(TopTracksNamespaceSessionTest, ServerReceivesFilterWithinItsLimit) {
  clientSession_->addExtension(std::make_shared<TopTracks>(0));
  serverSession_->addExtension(std::make_shared<TopTracks>(5));
  co_await setupMoQSession();
  const auto version = GetParam().serverVersion;

  std::optional<TrackFilter> received;
  EXPECT_CALL(*serverPublisher, subscribeNamespace(_, _))
      .WillOnce(
          [&received, version](auto subNs, auto /*handler*/)
              -> folly::coro::Task<Publisher::SubscribeNamespaceResult> {
            received = TopTracks::find(subNs.params, version);
            co_return std::make_shared<MockSubscribeNamespaceHandle>(
                SubscribeNamespaceOk(
                    {.requestID = subNs.requestID,
                     .requestSpecificParams = {}}));
          });
  EXPECT_CALL(*clientSubscriberStatsCallback_, onSubscribeNamespaceSuccess());
  EXPECT_CALL(*serverPublisherStatsCallback_, onSubscribeNamespaceSuccess());
  auto subNs = getSubscribeNamespace();
  CO_ASSERT_TRUE(
      subNs.params.insertParam(TopTracks::encode(TrackFilter{2, 5}, version))
          .hasValue());
  auto result = co_await clientSession_->subscribeNamespace(
      std::move(subNs),
      std::make_shared<testing::NiceMock<MockNamespacePublishHandle>>());
  CO_ASSERT_FALSE(result.hasError());
  EXPECT_EQ(received, (TrackFilter{2, 5}));
  clientSession_->close(SessionCloseErrorCode::NO_ERROR);
}

using TopTracksTracksSessionTest = MoQSessionTest;

INSTANTIATE_TEST_SUITE_P(
    TopTracksTracksSessionTest,
    TopTracksTracksSessionTest,
    testing::Values(
        VersionParams{{kVersionDraft18}, kVersionDraft18},
        VersionParams{{kVersionDraft22}, kVersionDraft22}));

CO_TEST_P_X(TopTracksTracksSessionTest, ServerReceivesFilterWithinItsLimit) {
  clientSession_->addExtension(std::make_shared<TopTracks>(0));
  serverSession_->addExtension(std::make_shared<TopTracks>(5));
  co_await setupMoQSession();
  const auto version = GetParam().serverVersion;

  std::optional<TrackFilter> received;
  std::shared_ptr<MockSubscribeTracksHandle> handle;
  EXPECT_CALL(*serverPublisher, subscribeTracks(_, _))
      .WillOnce(
          [&received, &handle, version](
              SubscribeTracks subTracks, auto /*publishBlockedHandle*/)
              -> folly::coro::Task<Publisher::SubscribeTracksResult> {
            received = TopTracks::find(subTracks.params, version);
            handle = std::make_shared<MockSubscribeTracksHandle>(
                SubscribeTracksOk({.requestID = subTracks.requestID}));
            co_return handle;
          });
  auto subTracks = getSubscribeTracks();
  CO_ASSERT_TRUE(subTracks.params
                     .insertParam(TopTracks::encode(TrackFilter{2, 5}, version))
                     .hasValue());
  auto result = co_await clientSession_->subscribeTracks(std::move(subTracks));
  CO_ASSERT_FALSE(result.hasError());
  EXPECT_EQ(received, (TrackFilter{2, 5}));

  folly::coro::Baton unsubscribed;
  EXPECT_CALL(*handle, unsubscribeTracks()).WillOnce([&unsubscribed]() {
    unsubscribed.post();
  });
  result.value()->unsubscribeTracks();
  co_await unsubscribed;
  clientSession_->close(SessionCloseErrorCode::NO_ERROR);
}
