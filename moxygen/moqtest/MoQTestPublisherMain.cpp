/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/coro/BlockingWait.h>
#include <folly/coro/Collect.h>
#include <folly/init/Init.h>
#include <folly/logging/xlog.h>
#include "moxygen/moqtest/MoQTestPublisher.h"
#include "moxygen/moqtest/Utils.h"
#include "moxygen/relay/MoQRelayClient.h"
#include "moxygen/samples/util/Utils.h"
#include "moxygen/util/InsecureVerifierDangerousDoNotUseInProduction.h"
#include "moxygen/util/SignalHandler.h"

DEFINE_string(url, "http://localhost:9999/moq-relay", "Relay URL");
DEFINE_string(
    track,
    "",
    "moq-test track namespace to PUBLISH, e.g. "
    "'moq-test-00/0/0/0/2/5/5/1024/100/50/1/1/0/-1/-1/0'. Repeat to publish "
    "more than one track. The namespace encodes the track parameters.");
DEFINE_string(
    ns_prefix,
    "",
    "Without --track, send PUBLISH_NAMESPACE for <ns_prefix>/moq-test-00 "
    "('/'-separated) and serve SUBSCRIBEs under it");
DEFINE_int32(connect_timeout, 1000, "Connect timeout (ms)");
DEFINE_int32(transaction_timeout, 120000, "Transaction timeout (ms)");
DEFINE_string(
    transport,
    "h3wt",
    "Transport: 'quic' (raw QUIC), 'h3wt' (HTTP/3 + WebTransport, default), "
    "'qmux' (QMUX-on-TCP, TLS via Fizz mandatory).");
DEFINE_string(
    versions,
    "",
    "Comma-separated MoQ draft versions (e.g. '14,16'). Empty = all supported.");
DEFINE_bool(
    include_timestamp_extension,
    false,
    "Stamp each object with a send-time millisecond timestamp extension.");

namespace {

// gflags has no native repeated-string flag, so collect --track from argv.
std::vector<std::string> collectTracks(int argc, char** argv) {
  std::vector<std::string> tracks;
  constexpr folly::StringPiece kPrefix{"--track="};
  for (int i = 1; i < argc; i++) {
    folly::StringPiece arg(argv[i]);
    if (arg.startsWith(kPrefix)) {
      arg.advance(kPrefix.size());
      tracks.emplace_back(arg.str());
    }
  }
  return tracks;
}

using TrackList =
    std::vector<std::pair<moxygen::FullTrackName, moxygen::MoQTestParameters>>;

// The publisher serves SUBSCRIBEs as the session's publish handler, so this
// only sends PUBLISH_NAMESPACE.
folly::coro::Task<bool> publishTestNamespace(
    std::shared_ptr<moxygen::MoQSession> session,
    std::string nsPrefix,
    std::shared_ptr<moxygen::Subscriber::PublishNamespaceHandle>& handle) {
  auto nsTuples = moxygen::parseNsPrefix(nsPrefix);
  nsTuples.emplace_back("moq-test-00");
  moxygen::PublishNamespace pubNs;
  pubNs.trackNamespace = moxygen::TrackNamespace(std::move(nsTuples));
  XLOG(INFO) << "PUBLISH_NAMESPACE " << pubNs.trackNamespace;
  auto res = co_await session->publishNamespace(std::move(pubNs));
  if (res.hasError()) {
    XLOG(ERR) << "PUBLISH_NAMESPACE failed: " << res.error().reasonPhrase;
    session->close(moxygen::SessionCloseErrorCode::NO_ERROR);
    co_return false;
  }
  handle = std::move(res.value());
  co_return true;
}

// Each publishTrack waits for the peer to turn forwarding on, so the tracks
// publish concurrently.
folly::coro::Task<bool> publishTracks(
    std::shared_ptr<moxygen::MoQTestPublisher> publisher,
    std::shared_ptr<moxygen::MoQSession> session,
    TrackList tracks) {
  std::vector<folly::coro::Task<void>> publishes;
  publishes.reserve(tracks.size());
  for (size_t i = 0; i < tracks.size(); i++) {
    XLOG(INFO) << "PUBLISH " << tracks[i].first.trackNamespace;
    publishes.emplace_back(publisher->publishTrack(
        session, tracks[i].first, tracks[i].second, moxygen::RequestID(i)));
  }
  bool ok = true;
  auto results = co_await folly::coro::collectAllTryRange(std::move(publishes));
  for (const auto& result : results) {
    // A cancelled publish means shutdown and does not count as a failure.
    if (result.hasException<folly::OperationCancelled>()) {
      continue;
    }
    if (result.hasException()) {
      ok = false;
      XLOG(ERR) << "PUBLISH failed: "
                << result.exception().what().toStdString();
    }
  }
  XLOG(INFO) << "All tracks done";
  session->drain();
  co_return ok;
}

} // namespace

int main(int argc, char** argv) {
  auto trackArgs = collectTracks(argc, argv);
  gflags::ParseCommandLineFlags(&argc, &argv, false);
  folly::Init init(&argc, &argv);

  if (!trackArgs.empty() && !FLAGS_ns_prefix.empty()) {
    XLOG(ERR) << "--ns_prefix applies only without --track";
    return 1;
  }

  // Decode up front so a bad namespace fails before we connect.
  TrackList tracks;
  for (const auto& trackArg : trackArgs) {
    moxygen::TrackNamespace ns(trackArg, "/");
    auto params = moxygen::convertTrackNamespaceToMoqTestParam(&ns);
    if (params.hasError()) {
      XLOG(ERR) << "Invalid --track=" << trackArg << ": "
                << params.error().what();
      return 1;
    }
    moxygen::FullTrackName ftn;
    ftn.trackNamespace = ns;
    ftn.trackName = "test";
    tracks.emplace_back(std::move(ftn), params.value());
  }

  // Not selectClientTransport(): that also requires the deprecated
  // --quic_transport flag, which this binary has no reason to carry.
  auto transportType = moxygen::samples::parseTransportType(FLAGS_transport);
  if (!transportType) {
    XLOG(ERR) << "Invalid --transport=" << FLAGS_transport
              << " (must be one of: quic, h3wt, qmux)";
    return 1;
  }

  proxygen::URL url(FLAGS_url);
  if (!url.isValid() || !url.hasHost()) {
    XLOG(ERR) << "Invalid url: " << FLAGS_url;
    return 1;
  }

  folly::EventBase evb;
  auto moqEvb = std::make_shared<moxygen::MoQFollyExecutorImpl>(&evb);
  auto publisher = std::make_shared<moxygen::MoQTestPublisher>();
  publisher->setIncludeTimestampExtension(FLAGS_include_timestamp_extension);

  auto relayClient = std::make_unique<moxygen::MoQRelayClient>(
      moxygen::samples::makeRelayClientTransport(
          moqEvb,
          std::move(url),
          moxygen::MoQRelaySession::createRelaySessionFactory(),
          std::make_shared<
              moxygen::test::InsecureVerifierDangerousDoNotUseInProduction>(),
          *transportType));

  // A signal stops the publishes mid-track and closes the session outright. The
  // loop keeps running so the cancelled publishes unwind before main returns.
  moxygen::SignalHandler signalHandler(
      &evb,
      [&](int) {
        publisher->cancelAll();
        if (auto session = relayClient->getSession()) {
          session->close(moxygen::SessionCloseErrorCode::NO_ERROR);
        }
      },
      /*terminateLoop=*/false);

  XLOG(INFO) << "Connecting to " << FLAGS_url;
  // Pass the EventBase so blockingWait drives it; the loop below has not
  // started yet, and without this the setup task would never be run.
  folly::coro::blockingWait(
      folly::coro::co_withExecutor(
          &evb,
          relayClient->setup(
              /*publisher=*/publisher,
              /*subscriber=*/nullptr,
              std::chrono::milliseconds(FLAGS_connect_timeout),
              std::chrono::milliseconds(FLAGS_transaction_timeout),
              quic::TransportSettings(),
              moxygen::getMoqtProtocols(FLAGS_versions, true))),
      &evb);

  auto session = relayClient->getSession();
  if (!session) {
    XLOG(ERR) << "Failed to establish a session with " << FLAGS_url;
    return 1;
  }

  // Once the session ends, drop the signal handler so evb.loop() can return.
  // Without --track, this serves until a signal or the idle timeout ends the
  // session.
  folly::CancellationCallback onSessionEnd(
      session->getCancelToken(), [&] { signalHandler.unregister(); });
  std::shared_ptr<moxygen::Subscriber::PublishNamespaceHandle> nsHandle;
  auto done = folly::coro::co_withExecutor(
                  &evb,
                  trackArgs.empty()
                      ? publishTestNamespace(session, FLAGS_ns_prefix, nsHandle)
                      : publishTracks(publisher, session, std::move(tracks)))
                  .start();
  evb.loop();
  return std::move(done).get() ? 0 : 1;
}
