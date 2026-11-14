/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <moxygen/openmoq/extensions/MoQClusterExtension.h>

#include <moxygen/test/MoQSessionTestCommon.h>
#include <moxygen/test/MockMoQSession.h>

using namespace moxygen;
using namespace moxygen::test;

// MoQSessionTestCommon declares these for MoQSessionTests.
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(Draft18Test);
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(Draft22Test);
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(PreDraft18Test);

namespace {

using Cluster = MoQClusterExtension;

SetupParameters setupWith(std::vector<SetupParameter> options) {
  SetupParameters params(FrameType::CLIENT_SETUP);
  for (auto& option : options) {
    EXPECT_TRUE(params.insertParam(std::move(option)).hasValue());
  }
  return params;
}

SetupParameter hopID(uint64_t id) {
  return SetupParameter(Cluster::kHopIdOption, id);
}

std::shared_ptr<const MessageParameterExtensions> negotiatedRules(
    uint64_t version = kVersionDraft18) {
  auto rules =
      Cluster(1).negotiate(version, setupWith({hopID(1)}), setupWith({hopID(2)}));
  EXPECT_TRUE(rules.hasValue());
  return std::make_shared<MessageParameterExtensions>(std::move(rules.value()));
}

Parameter hopPath(std::vector<uint64_t> path, uint64_t version) {
  return Parameter(
      Cluster::kHopPathParam, encodeRelayHopPath(path, version).value());
}

Parameter routeCost(uint64_t cost) {
  return Parameter(Cluster::kRouteCostParam, cost);
}

// Writes with `writeRules` and parses the result with `parseRules`; either
// step failing yields nullopt.
std::optional<PublishNamespace> roundTripPublishNamespace(
    std::vector<Parameter> params,
    std::shared_ptr<const MessageParameterExtensions> writeRules,
    std::shared_ptr<const MessageParameterExtensions> parseRules) {
  MoQFrameWriter writer;
  writer.initializeVersion(kVersionDraft18);
  writer.setMessageParameterExtensions(std::move(writeRules));
  PublishNamespace pubNs;
  pubNs.requestID = RequestID(0);
  pubNs.trackNamespace = TrackNamespace({"cluster"});
  for (auto& param : params) {
    EXPECT_TRUE(pubNs.params.insertParam(std::move(param)).hasValue());
  }
  folly::IOBufQueue buf{folly::IOBufQueue::cacheChainLength()};
  if (writer.writePublishNamespace(buf, pubNs).hasError()) {
    return std::nullopt;
  }
  auto serialized = buf.move();
  MoQFrameParser parser;
  parser.initializeVersion(kVersionDraft18);
  parser.setMessageParameterExtensions(std::move(parseRules));
  folly::io::Cursor cursor(serialized.get());
  EXPECT_TRUE(parser.decodeVarint(cursor).has_value());
  auto parsed = parser.parsePublishNamespace(cursor, cursor.readBE<uint16_t>());
  if (parsed.hasError()) {
    EXPECT_EQ(parsed.error(), ErrorCode::PROTOCOL_VIOLATION);
    return std::nullopt;
  }
  return std::move(parsed.value());
}

std::optional<RequestUpdate> roundTripRequestUpdate(
    std::vector<Parameter> params,
    std::shared_ptr<const MessageParameterExtensions> rules) {
  MoQFrameWriter writer;
  writer.initializeVersion(kVersionDraft18);
  writer.setMessageParameterExtensions(rules);
  RequestUpdate update;
  update.requestID = RequestID(2);
  for (auto& param : params) {
    EXPECT_TRUE(update.params.insertParam(std::move(param)).hasValue());
  }
  folly::IOBufQueue buf{folly::IOBufQueue::cacheChainLength()};
  if (writer.writeRequestUpdate(buf, update).hasError()) {
    return std::nullopt;
  }
  auto serialized = buf.move();
  MoQFrameParser parser;
  parser.initializeVersion(kVersionDraft18);
  parser.setMessageParameterExtensions(std::move(rules));
  folly::io::Cursor cursor(serialized.get());
  EXPECT_TRUE(parser.decodeVarint(cursor).has_value());
  auto parsed = parser.parseRequestUpdate(cursor, cursor.readBE<uint16_t>());
  if (parsed.hasError()) {
    return std::nullopt;
  }
  return std::move(parsed.value());
}

} // namespace

TEST(RelayHopPathTest, RoundTripsOnEveryVarintForm) {
  const std::vector<uint64_t> path{
      1, 63, 64, uint64_t{1} << 30, quic::kEightByteLimit};
  for (auto version : {kVersionDraft16, kVersionDraft18}) {
    SCOPED_TRACE(version);
    auto encoded = encodeRelayHopPath(path, version);
    ASSERT_TRUE(encoded.hasValue());
    auto decoded = decodeRelayHopPath(*encoded, version);
    ASSERT_TRUE(decoded.hasValue());
    EXPECT_EQ(*decoded, path);
  }
}

TEST(RelayHopPathTest, OnlyAnonymousHopsRepeat) {
  EXPECT_TRUE(encodeRelayHopPath({1, 2, 1}, kVersionDraft18).hasError());
  EXPECT_TRUE(
      decodeRelayHopPath(std::string("\x01\x02\x01", 3), kVersionDraft18)
          .hasError());
  auto anonymous = decodeRelayHopPath(std::string("\0\0", 2), kVersionDraft18);
  ASSERT_TRUE(anonymous.hasValue());
  EXPECT_EQ(*anonymous, (std::vector<uint64_t>{0, 0}));
}

TEST(RelayHopPathTest, RejectsEmptyAndTruncatedValues) {
  EXPECT_TRUE(encodeRelayHopPath({}, kVersionDraft18).hasError());
  EXPECT_TRUE(decodeRelayHopPath({}, kVersionDraft18).hasError());
  auto truncated = encodeRelayHopPath({7}, kVersionDraft18).value();
  truncated.append("\x80", 1);
  EXPECT_TRUE(decodeRelayHopPath(truncated, kVersionDraft18).hasError());
}

TEST(MoQClusterExtensionTest, SetupOptionsStartAtDraft18) {
  EXPECT_TRUE(Cluster(42).getSetupOptions(kVersionDraft16).empty());

  auto options = Cluster(42).getSetupOptions(kVersionDraft18);
  ASSERT_EQ(options.size(), 1);
  EXPECT_EQ(options[0].key, Cluster::kHopIdOption);
  EXPECT_EQ(options[0].asUint64, 42);

  options = Cluster(42, 0).getSetupOptions(kVersionDraft18);
  ASSERT_EQ(options.size(), 2);
  EXPECT_EQ(options[1].key, Cluster::kRelayCostOption);
  EXPECT_EQ(options[1].asUint64, 0);
}

TEST(MoQClusterExtensionTest, NegotiatesWhenBothDeclareHopID) {
  const auto both = setupWith({hopID(1)});
  const auto none = setupWith({});
  EXPECT_EQ(Cluster(1).negotiate(kVersionDraft18, both, both)->size(), 2);
  EXPECT_TRUE(Cluster(1).negotiate(kVersionDraft18, both, none)->empty());
  EXPECT_TRUE(Cluster(1).negotiate(kVersionDraft18, none, both)->empty());
  EXPECT_TRUE(Cluster(1).negotiate(kVersionDraft16, both, both)->empty());
}

TEST(MoQClusterExtensionTest, RepeatedPeerOptionIsProtocolViolation) {
  for (auto key : {Cluster::kHopIdOption, Cluster::kRelayCostOption}) {
    SCOPED_TRACE(key);
    auto peer = setupWith(
        {hopID(2), SetupParameter(key, uint64_t{3}), SetupParameter(key, 4)});
    auto result =
        Cluster(1).negotiate(kVersionDraft18, setupWith({hopID(1)}), peer);
    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(result.error(), SessionCloseErrorCode::PROTOCOL_VIOLATION);
  }
}

TEST(MoQClusterExtensionTest, PublishNamespaceCarriesOneHopPath) {
  auto rules = negotiatedRules();
  auto parsed = roundTripPublishNamespace(
      {hopPath({11, 22}, kVersionDraft18), routeCost(8)}, rules, rules);
  ASSERT_TRUE(parsed.has_value());
  const auto* path = parsed->params.getFirstParam(Cluster::kHopPathParam);
  ASSERT_NE(path, nullptr);
  EXPECT_EQ(
      decodeRelayHopPath(path->asString, kVersionDraft18).value(),
      (std::vector<uint64_t>{11, 22}));
  EXPECT_EQ(parsed->params.getFirstParam(Cluster::kRouteCostParam)->asUint64, 8);

  // The permissive rules let the bytes onto the wire, so the parse is tested.
  auto permissive = std::make_shared<MessageParameterExtensions>(*rules);
  for (auto& [key, rule] : *permissive) {
    rule.requiredFrames.clear();
    rule.repeatable = true;
    rule.validate = nullptr;
  }
  const std::vector<std::pair<std::string, std::vector<Parameter>>> invalid{
      {"missing", {routeCost(8)}},
      {"repeated",
       {hopPath({1}, kVersionDraft18), hopPath({2}, kVersionDraft18)}},
      {"repeated cost",
       {hopPath({1}, kVersionDraft18), routeCost(1), routeCost(2)}},
      {"malformed",
       {Parameter(Cluster::kHopPathParam, std::string("\x01\x01", 2))}},
  };
  for (const auto& [name, params] : invalid) {
    SCOPED_TRACE(name);
    EXPECT_FALSE(roundTripPublishNamespace(params, rules, rules).has_value());
    EXPECT_FALSE(
        roundTripPublishNamespace(params, permissive, rules).has_value());
  }
}

TEST(MoQClusterExtensionTest, NamespaceCarriesOneHopPath) {
  auto rules = negotiatedRules();
  auto write =
      [](std::shared_ptr<const MessageParameterExtensions> extensions,
         std::vector<Parameter> params) -> std::unique_ptr<folly::IOBuf> {
    MoQFrameWriter writer;
    writer.initializeVersion(kVersionDraft18);
    writer.setMessageParameterExtensions(std::move(extensions));
    Namespace ns;
    ns.trackNamespaceSuffix = TrackNamespace({"cluster"});
    for (auto& param : params) {
      EXPECT_TRUE(ns.params.insertParam(std::move(param)).hasValue());
    }
    folly::IOBufQueue buf{folly::IOBufQueue::cacheChainLength()};
    if (writer.writeNamespace(buf, ns).hasError()) {
      return nullptr;
    }
    return buf.move();
  };
  auto parse = [&rules](const folly::IOBuf& serialized) {
    MoQFrameParser parser;
    parser.initializeVersion(kVersionDraft18);
    parser.setMessageParameterExtensions(rules);
    folly::io::Cursor cursor(&serialized);
    EXPECT_TRUE(parser.decodeVarint(cursor).has_value());
    return parser.parseNamespace(cursor, cursor.readBE<uint16_t>());
  };

  auto serialized =
      write(rules, {hopPath({5, 6}, kVersionDraft18), routeCost(2)});
  ASSERT_NE(serialized, nullptr);
  auto parsed = parse(*serialized);
  ASSERT_TRUE(parsed.hasValue());
  EXPECT_EQ(
      decodeRelayHopPath(
          parsed->params.getFirstParam(Cluster::kHopPathParam)->asString,
          kVersionDraft18)
          .value(),
      (std::vector<uint64_t>{5, 6}));
  EXPECT_EQ(
      parsed->params.getFirstParam(Cluster::kRouteCostParam)->asUint64, 2);

  EXPECT_EQ(write(rules, {routeCost(2)}), nullptr);
  auto loose = std::make_shared<MessageParameterExtensions>(*rules);
  for (auto& [key, rule] : *loose) {
    rule.requiredFrames.clear();
  }
  auto missing = write(loose, {routeCost(2)});
  ASSERT_NE(missing, nullptr);
  auto rejected = parse(*missing);
  ASSERT_TRUE(rejected.hasError());
  EXPECT_EQ(rejected.error(), ErrorCode::PROTOCOL_VIOLATION);
}

TEST(MoQClusterExtensionTest, ParametersNeedNegotiation) {
  auto rules = negotiatedRules();
  const std::vector<Parameter> params{hopPath({1}, kVersionDraft18)};
  EXPECT_FALSE(roundTripPublishNamespace(params, nullptr, nullptr));
  EXPECT_FALSE(roundTripPublishNamespace(params, rules, nullptr));
}

TEST(MoQClusterExtensionTest, RequestUpdateMayOmitHopPath) {
  auto rules = negotiatedRules();
  auto costOnly = roundTripRequestUpdate({routeCost(0)}, rules);
  ASSERT_TRUE(costOnly.has_value());
  EXPECT_EQ(costOnly->params.getFirstParam(Cluster::kRouteCostParam)->asUint64, 0);
  EXPECT_FALSE(costOnly->params.hasParam(Cluster::kHopPathParam));

  auto both = roundTripRequestUpdate(
      {routeCost(0), hopPath({42, 99}, kVersionDraft18)}, rules);
  ASSERT_TRUE(both.has_value());
  EXPECT_TRUE(both->params.hasParam(Cluster::kHopPathParam));

  EXPECT_FALSE(roundTripRequestUpdate({routeCost(0)}, nullptr));
}

TEST(MoQClusterExtensionTest, MockSessionReportsPeer) {
  testing::NiceMock<MockMoQSession> session;
  EXPECT_FALSE(Cluster::isNegotiated(session));
  EXPECT_EQ(Cluster::peerHopID(session), kMoQClusterAnonHopId);
  EXPECT_EQ(Cluster::linkCost(session), 1);

  // Negotiated rules without a peer SETUP fall back rather than crash.
  session.messageParameterExtensions = *negotiatedRules();
  EXPECT_EQ(Cluster::peerHopID(session), kMoQClusterAnonHopId);
  EXPECT_EQ(Cluster::linkCost(session), 1);

  session.peerSetupParams =
      setupWith({hopID(7), SetupParameter(Cluster::kRelayCostOption, 3)});
  EXPECT_TRUE(Cluster::isNegotiated(session));
  EXPECT_EQ(Cluster::peerHopID(session), 7);
  EXPECT_EQ(Cluster::linkCost(session), 3);
}

using ClusterSessionTest = MoQSessionTest;

INSTANTIATE_TEST_SUITE_P(
    ClusterSessionTest,
    ClusterSessionTest,
    testing::Values(
        VersionParams{{kVersionDraft16}, kVersionDraft16},
        VersionParams{{kVersionDraft18}, kVersionDraft18}));

CO_TEST_P_X(ClusterSessionTest, PeersLearnEachOthersHopAndCost) {
  clientSession_->addExtension(std::make_shared<Cluster>(41, 0));
  serverSession_->addExtension(std::make_shared<Cluster>(23, 7));
  co_await setupMoQSession();
  const bool on = getDraftMajorVersion(GetParam().serverVersion) >= 18;
  EXPECT_EQ(Cluster::isNegotiated(*clientSession_), on);
  EXPECT_EQ(Cluster::isNegotiated(*serverSession_), on);
  EXPECT_EQ(Cluster::peerHopID(*clientSession_), on ? 23 : 0);
  EXPECT_EQ(Cluster::peerHopID(*serverSession_), on ? 41 : 0);
  EXPECT_EQ(Cluster::linkCost(*clientSession_), on ? 7 : 1);
  EXPECT_EQ(Cluster::linkCost(*serverSession_), on ? 0 : 1);
  clientSession_->close(SessionCloseErrorCode::NO_ERROR);
}

CO_TEST_P_X(ClusterSessionTest, OneSidedDeclarationStaysOff) {
  serverSession_->addExtension(std::make_shared<Cluster>(23));
  co_await setupMoQSession();
  EXPECT_FALSE(Cluster::isNegotiated(*clientSession_));
  EXPECT_FALSE(Cluster::isNegotiated(*serverSession_));
  EXPECT_EQ(Cluster::peerHopID(*serverSession_), kMoQClusterAnonHopId);
  clientSession_->close(SessionCloseErrorCode::NO_ERROR);
}
