/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

// Session tests for openmoq-only features, kept out of upstream's test files so
// a sync merge cannot drop them.

#include "moxygen/test/MoQSessionTestCommon.h"

using namespace moxygen;
using namespace moxygen::test;

// moqsessiontestcommon instantiates these for MoQSessionTests. This binary does
// not define any of their tests.
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(Draft18Test);
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(PreDraft18Test);

using OpenMOQSetupTest = MoQSessionTest;

INSTANTIATE_TEST_SUITE_P(
    OpenMOQSetupTest,
    OpenMOQSetupTest,
    testing::ValuesIn(getSupportedVersionParams()));

// Both halves of the setup exchange are retained on both endpoints, which is
// what extension negotiation runs on. Draft 18+ does not send MAX_REQUEST_ID,
// so the peer copy only carries it on earlier drafts.
TEST_P(OpenMOQSetupTest, SetupParamsRetainedOnBothEndpoints) {
  folly::coro::blockingWait(setupMoQSession(), getExecutor());
  const bool peerHasMaxRequestID =
      !useBidiRequestStreams(getServerSelectedVersion());
  for (auto* session : {clientSession_.get(), serverSession_.get()}) {
    ASSERT_TRUE(session->getLocalSetupParams().has_value());
    ASSERT_TRUE(session->getPeerSetupParams().has_value());
    EXPECT_TRUE(session->getLocalSetupParams()->hasParam(
        folly::to_underlying(SetupKey::MAX_REQUEST_ID)));
    EXPECT_EQ(
        session->getPeerSetupParams()->hasParam(
            folly::to_underlying(SetupKey::MAX_REQUEST_ID)),
        peerHasMaxRequestID);
  }
  clientSession_->close(SessionCloseErrorCode::NO_ERROR);
}

using RelayHopsNegotiationTest = MoQSessionTest;

INSTANTIATE_TEST_SUITE_P(
    RelayHopsNegotiationTest,
    RelayHopsNegotiationTest,
    testing::Values(
        VersionParams{{kVersionDraft16}, kVersionDraft16},
        VersionParams{{kVersionDraft18}, kVersionDraft18}));

CO_TEST_P_X(RelayHopsNegotiationTest, NegotiatesWhenBothPeersAdvertise) {
  relayHopsSupported_ = true;
  co_await setupMoQSession();
  EXPECT_TRUE(
      clientSession_->negotiatedSetupExtension(SetupExtension::RelayHops));
  EXPECT_TRUE(
      serverSession_->negotiatedSetupExtension(SetupExtension::RelayHops));
}

namespace {
class StubPublisher : public MoQSession::PublisherImpl {
 public:
  using PublisherImpl::PublisherImpl;
  void terminatePublish(PublishDone, ResetStreamErrorCode) override {}
  void sessionClosed(ResetStreamErrorCode) override {}
  void resetForGoaway(ResetStreamErrorCode) override {}
  void onStreamComplete(const ObjectHeader&) override {}
  bool hasOpenDataStreams() const override {
    return false;
  }
  void onTooManyBytesBuffered() override {}
};
} // namespace

// A publisher's writer, and the stream writers copied from it, are built after
// setup and must still carry the negotiated extensions.
CO_TEST_P_X(RelayHopsNegotiationTest, PublisherWriterTakesNegotiatedExtensions) {
  relayHopsSupported_ = true;
  co_await setupMoQSession();
  auto version = *serverSession_->getNegotiatedVersion();
  StubPublisher publisher(
      serverSession_.get(),
      FullTrackName{TrackNamespace{{"ns"}}, "track"},
      RequestID(0),
      kDefaultPriority,
      GroupOrder::OldestFirst,
      version,
      0);
  EXPECT_TRUE(
      publisher.getNegotiatedExtensions().has(SetupExtension::RelayHops));
}

CO_TEST_P_X(RelayHopsNegotiationTest, RemainsDisabledWithoutAdvertisement) {
  co_await setupMoQSession();
  EXPECT_FALSE(
      clientSession_->negotiatedSetupExtension(SetupExtension::RelayHops));
  EXPECT_FALSE(
      serverSession_->negotiatedSetupExtension(SetupExtension::RelayHops));
}

CO_TEST_P_X(RelayHopsNegotiationTest, RemainsDisabledWhenOnlyServerAdvertises) {
  serverRelayHopsSupported_ = true;
  co_await setupMoQSession();
  EXPECT_FALSE(
      clientSession_->negotiatedSetupExtension(SetupExtension::RelayHops));
  EXPECT_FALSE(
      serverSession_->negotiatedSetupExtension(SetupExtension::RelayHops));
}

CO_TEST_P_X(RelayHopsNegotiationTest, RemainsDisabledWhenOnlyClientAdvertises) {
  clientRelayHopsSupported_ = true;
  co_await setupMoQSession();
  EXPECT_FALSE(
      clientSession_->negotiatedSetupExtension(SetupExtension::RelayHops));
  EXPECT_FALSE(
      serverSession_->negotiatedSetupExtension(SetupExtension::RelayHops));
}

// Tests for MoQSession::computeNegotiatedExtensions().
class SetupExtensionsTest : public ::testing::Test {
 protected:
  static constexpr auto kExtA = static_cast<SetupExtension>(1u << 0);
  static constexpr auto kExtB = static_cast<SetupExtension>(1u << 1);
  // Synthetic keys for these tests; deliberately not any real SetupKey.
  static constexpr uint64_t kKeyA = 0xF00D1;
  static constexpr uint64_t kKeyB = 0xF00D2;
  static constexpr uint64_t kVersion = kVersionDraft18;

  const std::vector<SetupExtensionDescriptor> mutualFlags_{
      {kExtA, bothAdvertise(kKeyA)},
      {kExtB, bothAdvertise(kKeyB)}};

  static SetupParameters params(std::vector<SetupParameter> setupParams) {
    SetupParameters result(FrameType::CLIENT_SETUP);
    for (auto& param : setupParams) {
      result.insertParam(std::move(param));
    }
    return result;
  }

  static SetupParameter flag(uint64_t key) {
    return SetupParameter(key, std::string{});
  }
};

TEST_F(SetupExtensionsTest, MutualFlagNegotiatesWhenBothAdvertise) {
  auto extensions = MoQSession::computeNegotiatedExtensions(
      params({flag(kKeyA)}), params({flag(kKeyA)}), kVersion, mutualFlags_);
  EXPECT_TRUE(extensions.has(kExtA));
  EXPECT_FALSE(extensions.has(kExtB));
}

TEST_F(SetupExtensionsTest, MutualFlagNeedsBothSides) {
  EXPECT_TRUE(MoQSession::computeNegotiatedExtensions(
                  params({flag(kKeyA)}), params({}), kVersion, mutualFlags_)
                  .empty());
  EXPECT_TRUE(MoQSession::computeNegotiatedExtensions(
                  params({}), params({flag(kKeyA)}), kVersion, mutualFlags_)
                  .empty());
  EXPECT_TRUE(MoQSession::computeNegotiatedExtensions(
                  params({}), params({}), kVersion, mutualFlags_)
                  .empty());
}

TEST_F(SetupExtensionsTest, ExtensionsAreIndependent) {
  auto extensions = MoQSession::computeNegotiatedExtensions(
      params({flag(kKeyA), flag(kKeyB)}),
      params({flag(kKeyB)}),
      kVersion,
      mutualFlags_);
  EXPECT_FALSE(extensions.has(kExtA));
  EXPECT_TRUE(extensions.has(kExtB));
  EXPECT_FALSE(extensions.empty());
}

TEST_F(SetupExtensionsTest, UnrelatedSetupParamsDoNotNegotiate) {
  auto maxRequestID = params(
      {SetupParameter(folly::to_underlying(SetupKey::MAX_REQUEST_ID), 8)});
  EXPECT_TRUE(MoQSession::computeNegotiatedExtensions(
                  maxRequestID, maxRequestID, kVersion, mutualFlags_)
                  .empty());
}

// A rule can inspect values, be asymmetric, and depend on the draft -- the
// point of taking a negotiator rather than a key.
TEST_F(SetupExtensionsTest, RuleCanNegotiateOnValue) {
  const std::vector<SetupExtensionDescriptor> byValue{
      {kExtA,
       [](const SetupParameters& local,
          const SetupParameters& peer,
          uint64_t) {
         const auto* localParam = local.getFirstParam(kKeyA);
         const auto* peerParam = peer.getFirstParam(kKeyA);
         return localParam && peerParam &&
             std::min(localParam->asUint64, peerParam->asUint64) > 0;
       }}};

  EXPECT_TRUE(MoQSession::computeNegotiatedExtensions(
                  params({SetupParameter(kKeyA, 4)}),
                  params({SetupParameter(kKeyA, 2)}),
                  kVersion,
                  byValue)
                  .has(kExtA));
  EXPECT_FALSE(MoQSession::computeNegotiatedExtensions(
                   params({SetupParameter(kKeyA, 4)}),
                   params({SetupParameter(kKeyA, 0)}),
                   kVersion,
                   byValue)
                   .has(kExtA));
}

// Modelled on the auth token cache: my advertisement governs what I receive,
// the peer's governs what I send, so the two directions can differ.
TEST_F(SetupExtensionsTest, RuleCanBeAsymmetric) {
  const std::vector<SetupExtensionDescriptor> perDirection{
      {kExtA,
       [](const SetupParameters& local, const SetupParameters&, uint64_t) {
         return local.hasParam(kKeyA);
       }},
      {kExtB,
       [](const SetupParameters&, const SetupParameters& peer, uint64_t) {
         return peer.hasParam(kKeyA);
       }}};

  auto extensions = MoQSession::computeNegotiatedExtensions(
      params({flag(kKeyA)}), params({}), kVersion, perDirection);
  EXPECT_TRUE(extensions.has(kExtA));
  EXPECT_FALSE(extensions.has(kExtB));
}

TEST_F(SetupExtensionsTest, RuleCanDependOnVersion) {
  const std::vector<SetupExtensionDescriptor> v18Only{
      {kExtA,
       [](const SetupParameters& local,
          const SetupParameters& peer,
          uint64_t version) {
         return getDraftMajorVersion(version) >= 18 && local.hasParam(kKeyA) &&
             peer.hasParam(kKeyA);
       }}};

  auto both = params({flag(kKeyA)});
  EXPECT_TRUE(MoQSession::computeNegotiatedExtensions(
                  both, both, kVersionDraft18, v18Only)
                  .has(kExtA));
  EXPECT_FALSE(MoQSession::computeNegotiatedExtensions(
                   both, both, kVersionDraft17, v18Only)
                   .has(kExtA));
}

TEST_F(SetupExtensionsTest, NoneIsNeverHeld) {
  auto extensions = MoQSession::computeNegotiatedExtensions(
      params({flag(kKeyA)}), params({flag(kKeyA)}), kVersion, mutualFlags_);
  EXPECT_FALSE(extensions.has(SetupExtension::None));
  EXPECT_FALSE(SetupExtensions().has(SetupExtension::None));
}

TEST_F(SetupExtensionsTest, ShippedTableNegotiatesRelayHops) {
  auto both = params({flag(folly::to_underlying(SetupKey::RELAY_HOPS))});
  auto relayHops =
      MoQSession::computeNegotiatedExtensions(both, both, kVersion);
  EXPECT_TRUE(relayHops.has(SetupExtension::RelayHops));

  auto oneSided =
      MoQSession::computeNegotiatedExtensions(both, params({}), kVersion);
  EXPECT_FALSE(oneSided.has(SetupExtension::RelayHops));

  auto draft17 =
      MoQSession::computeNegotiatedExtensions(both, both, kVersionDraft17);
  EXPECT_TRUE(draft17.has(SetupExtension::RelayHops));

  auto draft15 =
      MoQSession::computeNegotiatedExtensions(both, both, kVersionDraft15);
  EXPECT_FALSE(draft15.has(SetupExtension::RelayHops));
}
