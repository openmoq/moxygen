/*
 * Copyright (c) Synamedia
 * SPDX-License-Identifier: Apache-2.0
 */

#include <folly/coro/BlockingWait.h>
#include <folly/coro/Collect.h>
#include <folly/coro/Sleep.h>
#include <folly/init/Init.h>
#include <folly/io/async/EventBaseManager.h>
#include <folly/portability/GFlags.h>
#include <moxygen/MoQWebTransportClient.h>
#include <moxygen/events/MoQFollyExecutorImpl.h>
#include <moxygen/relay/MoQForwarder.h>
#include <moxygen/relay/MoQRelayClient.h>
#include <moxygen/util/InsecureVerifierDangerousDoNotUseInProduction.h>

DEFINE_string(
    relay_url,
    "https://localhost:4433/moq-relay",
    "Relay WebTransport URL");
DEFINE_string(ns, "test", "Track namespace");
DEFINE_int32(interval_ms, 100, "Group publish interval in ms");
DEFINE_bool(insecure, false, "Skip TLS certificate validation");

namespace {
using namespace moxygen;

class SwitchPublisher : public std::enable_shared_from_this<SwitchPublisher> {
 public:
  explicit SwitchPublisher(std::string ns)
      : ns_(std::move(ns)),
        highForwarder_(FullTrackName{TrackNamespace({ns_}), "high"}),
        lowForwarder_(FullTrackName{TrackNamespace({ns_}), "low"}) {}

  folly::coro::Task<void> publishTrack(
      std::shared_ptr<MoQRelayClient> client,
      MoQForwarder& forwarder,
      const std::string& trackName,
      uint64_t requestID) {
    auto session = client->getSession();
    PublishRequest req;
    req.requestID = RequestID{requestID};
    req.fullTrackName = FullTrackName{TrackNamespace({ns_}), trackName};
    req.trackAlias = TrackAlias(requestID);
    req.groupOrder = GroupOrder::OldestFirst;
    req.forward = true;

    auto handle = forwarder.addSubscriber(session, req.forward);
    if (!handle) {
      XLOG(ERR) << "addSubscriber failed for " << trackName;
      co_return;
    }
    auto guard = folly::makeGuard([&] { handle->unsubscribe(); });

    auto publishRes = session->publish(req, handle);
    if (!publishRes.hasValue()) {
      XLOG(ERR) << "publish() failed for " << trackName << ": "
                << publishRes.error().reasonPhrase;
      co_return;
    }
    handle->trackConsumer = std::move(publishRes.value().consumer);

    auto reply = co_await co_awaitTry(std::move(publishRes.value().reply));
    if (reply.hasException()) {
      XLOG(ERR) << "PUBLISH_OK exception for " << trackName;
      co_return;
    }
    if (reply.value().hasError()) {
      XLOG(ERR) << "PUBLISH_OK error for " << trackName << ": "
                << reply.value().error().reasonPhrase;
      co_return;
    }
    guard.dismiss();
    handle->onPublishOk(reply.value().value());
  }

  void tick(uint64_t groupID) {
    for (auto* fwd : {&highForwarder_, &lowForwarder_}) {
      if (fwd->empty()) {
        continue;
      }
      auto sg = fwd->beginSubgroup(groupID, /*subgroupID=*/0, /*priority=*/0,
                                   /*containsLastInGroup=*/true);
      if (!sg) {
        continue;
      }
      auto& ftn = fwd->fullTrackName();
      auto payload =
          folly::to<std::string>(ftn.trackName, ":g=", groupID, ":o=0");
      (*sg)->object(
          /*objectID=*/0,
          folly::IOBuf::copyBuffer(payload),
          noExtensions(),
          /*finSubgroup=*/true);
    }
  }

  MoQForwarder& highForwarder() {
    return highForwarder_;
  }
  MoQForwarder& lowForwarder() {
    return lowForwarder_;
  }

 private:
  std::string ns_;
  MoQForwarder highForwarder_;
  MoQForwarder lowForwarder_;
};

folly::coro::Task<void> run(std::shared_ptr<SwitchPublisher> pub) {
  auto executor = std::make_shared<MoQFollyExecutorImpl>(
      folly::EventBaseManager::get()->getEventBase());
  proxygen::URL url(FLAGS_relay_url);

  std::shared_ptr<fizz::CertificateVerifier> verifier;
  if (FLAGS_insecure) {
    verifier =
        std::make_shared<test::InsecureVerifierDangerousDoNotUseInProduction>();
  }

  auto client = std::make_shared<MoQRelayClient>(
      std::make_unique<MoQWebTransportClient>(executor, url, verifier));

  co_await client->setup(
      /*publisher=*/nullptr,
      /*subscriber=*/nullptr,
      std::chrono::milliseconds(2000),
      std::chrono::seconds(120));

  // Launch PUBLISH for both tracks concurrently
  co_await folly::coro::collectAll(
      pub->publishTrack(client, pub->highForwarder(), "high", 1),
      pub->publishTrack(client, pub->lowForwarder(), "low", 2));

  // Tick loop — publish one group per interval on both tracks
  uint64_t groupID = 0;
  while (true) {
    pub->tick(groupID++);
    co_await folly::coro::sleep(std::chrono::milliseconds(FLAGS_interval_ms));
  }
}

} // namespace

int main(int argc, char* argv[]) {
  folly::Init init(&argc, &argv);
  auto pub = std::make_shared<SwitchPublisher>(FLAGS_ns);
  folly::coro::blockingWait(run(pub));
  return 0;
}
