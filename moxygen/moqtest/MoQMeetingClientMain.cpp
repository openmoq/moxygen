/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <unistd.h>
#include <ctime>
#include <iomanip>
#include <memory>
#include <sstream>
#include <vector>

#include <folly/coro/BlockingWait.h>
#include <folly/coro/Sleep.h>
#include <folly/executors/IOThreadPoolExecutor.h>
#include <folly/init/Init.h>
#include <folly/io/async/ScopedEventBaseThread.h>
#include <folly/logging/xlog.h>

#include "moxygen/moqtest/MoQMeetingClient.h"
#include "moxygen/moqtest/MoQTestPublisher.h"
#include "moxygen/moqtest/PromMetrics.h"
#include "moxygen/samples/util/Utils.h"

DEFINE_string(relay_url, "https://localhost:9999", "Relay URL to connect to");
DEFINE_string(
    transport,
    "h3wt",
    "Client transport: 'quic' (raw QUIC), 'h3wt' (HTTP/3 + WebTransport, "
    "default), 'qmux' (QMUX-on-TCP, TLS via Fizz mandatory).");
DEFINE_string(
    versions,
    "",
    "Comma-separated MoQ draft versions (e.g. '14,16'). Empty = all supported.");
DEFINE_uint32(num_threads, 1, "Number of client threads");
DEFINE_uint32(duration, 60, "Test duration in seconds");
DEFINE_uint32(participants, 5, "Participants per meeting");
DEFINE_bool(
    colocate_meetings,
    false,
    "Run all of a meeting's participants on one thread, instead of spreading "
    "them across threads");
DEFINE_uint32(meeting_ramp, 5, "Max new meetings per second");
DEFINE_uint32(meeting_max, 100, "Max concurrent meetings");
DEFINE_string(
    meeting_prefix,
    "",
    "Prefix for meeting IDs.  Empty = derived from the pid and start time, so "
    "repeated runs don't collide in the relay");
DEFINE_uint32(
    join_grace_ms,
    3000,
    "How long a meeting has to connect and see every peer's tracks flowing");
DEFINE_uint32(
    join_jitter_ms,
    50,
    "Max random delay before each participant connects");
DEFINE_uint32(
    delivery_timeout,
    500,
    "Delivery timeout in ms requested on every received track; also the "
    "latency past which an audio datagram counts as late");
DEFINE_uint32(
    audio_loss_tolerance,
    0,
    "Lost or late audio datagrams a track tolerates before its meeting is "
    "cancelled");
DEFINE_uint32(
    video_first_object_size,
    7576,
    "Size of the first object in a video group (I-frame)");
DEFINE_uint32(
    video_object_size,
    1894,
    "Size of the other objects in a video group (P-frame)");
DEFINE_uint32(video_objects_per_group, 30, "Video objects per group");
DEFINE_uint32(video_object_interval_ms, 33, "Interval between video objects");
DEFINE_uint32(audio_object_size, 160, "Size of an audio datagram");
DEFINE_uint32(audio_interval_ms, 20, "Interval between audio datagrams");
DEFINE_uint32(connect_timeout, 1000, "Connect timeout in ms");
DEFINE_uint32(
    transaction_timeout,
    60000,
    "Transaction timeout in ms.  For WebTransport this is the idle timeout on "
    "the CONNECT stream, so a short value ends quiet sessions");
DEFINE_string(
    metrics_out,
    "",
    "If set, write Prometheus .prom metrics (including the end-to-end latency "
    "histograms) to this path once per second, for a node_exporter textfile "
    "collector to scrape");

namespace {

using moxygen::AtomicLatency;
using moxygen::MeetingWorker;
using moxygen::MoQMeetingClient;

constexpr double kBitsPerMbit = 1024.0 * 1024.0;

// Video goes out on subgroups and audio as datagrams, so the publisher's two
// SendStats split the same way.
struct TxTotals {
  uint64_t objects{0};
  uint64_t bytes{0};
  uint64_t late{0};
};

struct TrafficTotals {
  uint64_t videoObjects{0};
  uint64_t videoBytes{0};
  uint64_t audioDatagrams{0};
  uint64_t audioBytes{0};
  uint64_t resets{0};
  uint64_t audioLost{0};
  uint64_t audioLate{0};
  TxTotals txVideo;
  TxTotals txAudio;
};

void addTx(TxTotals& t, const moxygen::MoQTestPublisher::SendStats& s) {
  t.objects += s.objects.load(std::memory_order_relaxed);
  t.bytes += s.bytes.load(std::memory_order_relaxed);
  t.late += s.late.load(std::memory_order_relaxed);
}

TrafficTotals sumTraffic(
    const std::vector<std::unique_ptr<MeetingWorker>>& ws) {
  TrafficTotals t;
  for (const auto& w : ws) {
    const auto& s = w->stats;
    t.videoObjects += s.videoObjects.load(std::memory_order_relaxed);
    t.videoBytes += s.videoBytes.load(std::memory_order_relaxed);
    t.audioDatagrams += s.audioDatagrams.load(std::memory_order_relaxed);
    t.audioBytes += s.audioBytes.load(std::memory_order_relaxed);
    t.resets += s.resets.load(std::memory_order_relaxed);
    t.audioLost += s.audioLost.load(std::memory_order_relaxed);
    t.audioLate += s.audioLate.load(std::memory_order_relaxed);
    addTx(t.txVideo, w->publisher->subgroupStats());
    addTx(t.txAudio, w->publisher->datagramStats());
  }
  return t;
}

std::string formatInterval(const AtomicLatency::Interval& ivl) {
  if (ivl.count == 0) {
    return "-";
  }
  return fmt::format(
      "{}/{:.1f}/{}",
      ivl.minMs,
      static_cast<double>(ivl.sumMs) / static_cast<double>(ivl.count),
      ivl.maxMs);
}

std::string formatFailures(const MoQMeetingClient::Results& r) {
  std::string out;
  for (size_t i = 0; i < moxygen::kNumMeetingFailures; ++i) {
    out += fmt::format(
        "{}{} {}",
        i == 0 ? "" : ", ",
        moxygen::meetingFailureName(static_cast<moxygen::MeetingFailure>(i)),
        r.failures[i]);
  }
  return out;
}

void writePromFile(
    const std::string& path,
    const std::string& labels,
    const MoQMeetingClient::Results& r,
    const TrafficTotals& t,
    double rxMbps,
    double txMbps,
    const std::vector<std::unique_ptr<MeetingWorker>>& workers) {
  moxygen::PromWriter w(labels);
  w.gauge("moqmeeting_meetings", "Active meetings", r.activeMeetings);
  w.gauge(
      "moqmeeting_meetings_joined",
      "Meetings whose participants all see each other",
      r.joinedMeetings);
  w.gauge(
      "moqmeeting_participants",
      "Participants publishing",
      r.publishingParticipants);
  w.gauge("moqmeeting_rx_mbps", "Interval receive throughput in Mbps", rxMbps);
  w.gauge("moqmeeting_tx_mbps", "Interval send throughput in Mbps", txMbps);
  w.counter(
      "moqmeeting_meetings_started_total",
      "Meetings started",
      r.meetingsStarted);
  for (size_t i = 0; i < moxygen::kNumMeetingFailures; ++i) {
    auto reason =
        moxygen::meetingFailureName(static_cast<moxygen::MeetingFailure>(i));
    w.counter(
        fmt::format("moqmeeting_failures_{}_total", reason),
        fmt::format("Meetings cancelled for {}", reason),
        r.failures[i]);
  }
  w.counter(
      "moqmeeting_video_objects_total",
      "Video objects received",
      t.videoObjects);
  w.counter(
      "moqmeeting_video_bytes_total", "Video bytes received", t.videoBytes);
  w.counter(
      "moqmeeting_audio_datagrams_total",
      "Audio datagrams received",
      t.audioDatagrams);
  w.counter(
      "moqmeeting_audio_bytes_total", "Audio bytes received", t.audioBytes);
  w.counter("moqmeeting_resets_total", "Video subgroup resets", t.resets);
  w.counter("moqmeeting_audio_lost_total", "Audio datagrams lost", t.audioLost);
  w.counter("moqmeeting_audio_late_total", "Audio datagrams late", t.audioLate);
  for (const auto& [kind, tx] :
       {std::pair{"video", &t.txVideo}, std::pair{"audio", &t.txAudio}}) {
    w.counter(
        fmt::format("moqmeeting_tx_{}_objects_total", kind),
        fmt::format("{} objects sent", kind),
        tx->objects);
    w.counter(
        fmt::format("moqmeeting_tx_{}_bytes_total", kind),
        fmt::format("{} bytes sent", kind),
        tx->bytes);
    w.counter(
        fmt::format("moqmeeting_tx_{}_late_total", kind),
        fmt::format("{} objects sent past their deadline", kind),
        tx->late);
  }

  moxygen::LatencyHistogram video;
  moxygen::LatencyHistogram audio;
  for (const auto& worker : workers) {
    video.merge(worker->stats.videoLatency.snapshot());
    audio.merge(worker->stats.audioLatency.snapshot());
  }
  w.histogram(
      "moqmeeting_object_latency_seconds",
      "End-to-end object latency in seconds",
      {{"kind=\"video\"", video}, {"kind=\"audio\"", audio}});
  w.writeFile(path);
}

folly::coro::Task<void> aggregateStats(
    const MoQMeetingClient& client,
    const std::vector<std::unique_ptr<MeetingWorker>>& workers,
    folly::CancellationToken cancelToken,
    std::string metricsOut,
    std::string promLabels) {
  const auto& cfg = client.config();
  const uint64_t tracksPerMeeting =
      2ULL * cfg.participants * (cfg.participants - 1);
  auto startTime = std::chrono::steady_clock::now();
  TrafficTotals last;

  while (!cancelToken.isCancellationRequested()) {
    co_await folly::coro::sleepReturnEarlyOnCancel(std::chrono::seconds(1));

    auto r = client.getResults();
    auto t = sumTraffic(workers);
    AtomicLatency::Interval video;
    AtomicLatency::Interval audio;
    for (const auto& w : workers) {
      video.merge(w->stats.videoLatency.takeInterval());
      audio.merge(w->stats.audioLatency.takeInterval());
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::steady_clock::now() - startTime)
                       .count();
    double rxMbps =
        ((t.videoBytes - last.videoBytes) + (t.audioBytes - last.audioBytes)) *
        8.0 / kBitsPerMbit;
    double txMbps = ((t.txVideo.bytes - last.txVideo.bytes) +
                     (t.txAudio.bytes - last.txAudio.bytes)) *
        8.0 / kBitsPerMbit;

    XLOG(INFO) << "[AGGREGATE] [" << elapsed
               << "s] Meetings: " << r.joinedMeetings << " joined/"
               << r.activeMeetings << " active (started " << r.meetingsStarted
               << ")"
               << " | Failed: " << formatFailures(r)
               << " | Participants: " << r.publishingParticipants
               << " | RxTracks: " << r.rxTracks << "/"
               << r.activeMeetings * tracksPerMeeting
               << " | Video obj/s rx: " << t.videoObjects - last.videoObjects
               << " tx: " << t.txVideo.objects - last.txVideo.objects
               << " | Audio dg/s rx: " << t.audioDatagrams - last.audioDatagrams
               << " tx: " << t.txAudio.objects - last.txAudio.objects
               << " | Mbps rx: " << fmt::format("{:.2f}", rxMbps)
               << " tx: " << fmt::format("{:.2f}", txMbps)
               << " | Tx late/s video: " << t.txVideo.late - last.txVideo.late
               << ", audio: " << t.txAudio.late - last.txAudio.late
               << " | Latency(interval min/avg/max) video: "
               << formatInterval(video)
               << " ms, audio: " << formatInterval(audio) << " ms"
               << " | Resets: " << t.resets - last.resets << "/s, " << t.resets
               << " total"
               << " | Audio lost/late: " << t.audioLost << "/" << t.audioLate;

    if (!metricsOut.empty()) {
      writePromFile(metricsOut, promLabels, r, t, rxMbps, txMbps, workers);
    }
    last = t;
  }
}

std::string defaultMeetingPrefix() {
  return fmt::format("mtg{}x{}", getpid(), time(nullptr) % 1000000);
}

} // namespace

int main(int argc, char** argv) {
  gflags::ParseCommandLineFlags(&argc, &argv, false);
  folly::Init init(&argc, &argv);
  auto parsedTransport = moxygen::samples::parseTransportType(FLAGS_transport);
  if (!parsedTransport) {
    XLOG(ERR) << "Invalid --transport: " << FLAGS_transport;
    return 1;
  }
  auto transportType = *parsedTransport;
  const char* transportName =
      transportType == moxygen::samples::TransportType::QMUX   ? "QMUX"
      : transportType == moxygen::samples::TransportType::QUIC ? "QUIC"
                                                               : "WebTransport";

  if (FLAGS_num_threads == 0 || FLAGS_participants == 0 ||
      FLAGS_meeting_ramp == 0 || FLAGS_video_objects_per_group == 0) {
    XLOG(ERR) << "--num_threads, --participants, --meeting_ramp and "
                 "--video_objects_per_group must be at least 1";
    return 1;
  }

  moxygen::MeetingConfig cfg;
  cfg.url = proxygen::URL(FLAGS_relay_url);
  cfg.transportType = transportType;
  cfg.versions = FLAGS_versions;
  cfg.participants = FLAGS_participants;
  cfg.deliveryTimeoutMs = FLAGS_delivery_timeout;
  cfg.joinGraceMs = FLAGS_join_grace_ms;
  cfg.joinJitterMs = FLAGS_join_jitter_ms;
  cfg.audioLossTolerance = FLAGS_audio_loss_tolerance;
  cfg.videoFirstObjectSize = FLAGS_video_first_object_size;
  cfg.videoObjectSize = FLAGS_video_object_size;
  cfg.videoObjectsPerGroup = FLAGS_video_objects_per_group;
  cfg.videoObjectIntervalMs = FLAGS_video_object_interval_ms;
  cfg.audioObjectSize = FLAGS_audio_object_size;
  cfg.audioIntervalMs = FLAGS_audio_interval_ms;
  cfg.connectTimeoutMs = FLAGS_connect_timeout;
  cfg.transactionTimeoutMs = FLAGS_transaction_timeout;

  MoQMeetingClient::RampConfig ramp;
  ramp.durationSeconds = FLAGS_duration;
  ramp.meetingRamp = FLAGS_meeting_ramp;
  ramp.meetingMax = FLAGS_meeting_max;
  ramp.colocate = FLAGS_colocate_meetings;
  ramp.meetingPrefix = FLAGS_meeting_prefix.empty() ? defaultMeetingPrefix()
                                                    : FLAGS_meeting_prefix;

  const uint64_t n = cfg.participants;
  XLOG(INFO) << "MoQ Meeting Load Client";
  XLOG(INFO) << "Relay URL: " << FLAGS_relay_url << " (" << transportName
             << ", versions: "
             << (FLAGS_versions.empty() ? "all" : FLAGS_versions) << ")";
  XLOG(INFO) << "Threads: " << FLAGS_num_threads << " ("
             << (ramp.colocate ? "meetings colocated"
                               : "participants spread across threads")
             << ")";
  XLOG(INFO) << "Meetings: ramp " << ramp.meetingRamp << "/s, max "
             << ramp.meetingMax << ", " << n << " participants each, prefix "
             << ramp.meetingPrefix;
  XLOG(INFO) << "Per meeting the relay receives " << 2 * n
             << " tracks and sends " << 2 * n * (n - 1);
  XLOG(INFO) << "Video: " << cfg.videoFirstObjectSize << "/"
             << cfg.videoObjectSize << " bytes, " << cfg.videoObjectsPerGroup
             << " objects/group every " << cfg.videoObjectIntervalMs
             << " ms; audio: " << cfg.audioObjectSize << " bytes every "
             << cfg.audioIntervalMs << " ms";
  XLOG(INFO) << "Delivery timeout: " << cfg.deliveryTimeoutMs
             << " ms, join grace: " << cfg.joinGraceMs << " ms";

  std::ostringstream labelStream;
  labelStream << "transport=\"" << moxygen::escapeLabelValue(transportName)
              << "\""
              << ",participants=\"" << cfg.participants << "\""
              << ",meeting_max=\"" << ramp.meetingMax << "\""
              << ",colocate=\"" << (ramp.colocate ? "true" : "false") << "\""
              << ",versions=\""
              << moxygen::escapeLabelValue(
                     FLAGS_versions.empty() ? "all" : FLAGS_versions)
              << "\"";
  std::string promLabels = labelStream.str();

  auto executor = std::make_unique<folly::IOThreadPoolExecutor>(
      FLAGS_num_threads,
      std::make_shared<folly::NamedThreadFactory>("MoQMeeting"),
      folly::EventBaseManager::get(),
      folly::IOThreadPoolExecutor::Options().setWaitForAll(true));
  std::vector<std::unique_ptr<MeetingWorker>> workers;
  std::vector<MeetingWorker*> workerPtrs;
  for (auto& evb : executor->getAllEventBases()) {
    workers.push_back(std::make_unique<MeetingWorker>(evb.get()));
    workerPtrs.push_back(workers.back().get());
  }

  folly::ScopedEventBaseThread controllerThread("MeetingCtl");
  MoQMeetingClient client(
      controllerThread.getEventBase(), workerPtrs, cfg, std::move(ramp));

  folly::CancellationSource statsCancel;
  {
    folly::ScopedEventBaseThread statsThread("MeetingStats");
    folly::coro::co_withExecutor(
        statsThread.getEventBase(),
        aggregateStats(
            client,
            workers,
            statsCancel.getToken(),
            FLAGS_metrics_out,
            promLabels))
        .start();

    folly::coro::blockingWait(folly::coro::co_withExecutor(
        controllerThread.getEventBase(), client.run()));
    statsCancel.requestCancellation();
  }

  // A participant whose meeting did not finish ending is still registered.
  // Release it on its own thread.
  for (auto& w : workers) {
    w->evb->runInEventBaseThreadAndWait([&w] { w->shutdown(); });
  }
  executor->stop();

  auto r = client.getResults();
  auto t = sumTraffic(workers);
  moxygen::LatencyHistogram video;
  moxygen::LatencyHistogram audio;
  for (const auto& w : workers) {
    video.merge(w->stats.videoLatency.snapshot());
    audio.merge(w->stats.audioLatency.snapshot());
  }
  auto avg = [](const moxygen::LatencyHistogram& h) {
    return h.count() > 0
        ? static_cast<double>(h.sum()) / static_cast<double>(h.count())
        : 0.0;
  };
  double rxMbps = FLAGS_duration > 0
      ? (t.videoBytes + t.audioBytes) * 8.0 / kBitsPerMbit / FLAGS_duration
      : 0.0;
  const uint64_t txBytes = t.txVideo.bytes + t.txAudio.bytes;
  double txMbps =
      FLAGS_duration > 0 ? txBytes * 8.0 / kBitsPerMbit / FLAGS_duration : 0.0;
  auto formatTx = [](const TxTotals& tx) {
    return fmt::format(
        "{} objects, {} bytes, {} late", tx.objects, tx.bytes, tx.late);
  };

  XLOG(INFO) << "========================================";
  XLOG(INFO) << "Final Summary:";
  XLOG(INFO) << "  Meetings: " << r.meetingsStarted << " started, peak "
             << r.peakMeetings << " concurrent";
  XLOG(INFO) << "  Cancelled: " << formatFailures(r);
  XLOG(INFO) << "  Video: " << t.videoObjects << " objects, " << t.videoBytes
             << " bytes, avg latency " << fmt::format("{:.1f}", avg(video))
             << " ms";
  XLOG(INFO) << "  Audio: " << t.audioDatagrams << " datagrams, "
             << t.audioBytes << " bytes, avg latency "
             << fmt::format("{:.1f}", avg(audio)) << " ms, " << t.audioLost
             << " lost, " << t.audioLate << " late";
  XLOG(INFO) << "  Resets: " << t.resets;
  XLOG(INFO) << "  Self-echo tracks: " << r.selfEchoTracks;
  XLOG(INFO) << "  Video sent: " << formatTx(t.txVideo);
  XLOG(INFO) << "  Audio sent: " << formatTx(t.txAudio);
  XLOG(INFO) << "  Avg throughput rx: " << fmt::format("{:.2f}", rxMbps)
             << " Mbps, tx: " << fmt::format("{:.2f}", txMbps) << " Mbps";

  if (!FLAGS_metrics_out.empty()) {
    writePromFile(FLAGS_metrics_out, promLabels, r, t, rxMbps, txMbps, workers);
  }
  return 0;
}
