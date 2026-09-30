/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <iomanip>
#include <memory>
#include <sstream>
#include <vector>

#include <folly/coro/Sleep.h>
#include <folly/executors/IOThreadPoolExecutor.h>
#include <folly/init/Init.h>
#include <folly/io/async/ScopedEventBaseThread.h>
#include <folly/logging/xlog.h>

#include "moxygen/moqtest/MoQPerfTestClient.h"
#include "moxygen/moqtest/PromMetrics.h"
#include "moxygen/samples/util/Utils.h"

// Declared in MoQPerfTestClient.cpp.
DECLARE_string(versions);

DEFINE_string(relay_url, "https://localhost:9999", "Relay URL to connect to");
DEFINE_string(
    transport,
    "h3wt",
    "Client transport: 'quic' (raw QUIC), 'h3wt' (HTTP/3 + WebTransport, "
    "default), 'qmux' (QMUX-on-TCP, TLS via Fizz mandatory).");
DEFINE_bool(
    quic_transport,
    false,
    "DEPRECATED: use --transport=quic (or --transport=h3wt) instead. "
    "Selects raw QUIC vs WebTransport.");
DEFINE_uint32(num_threads, 1, "Number of client threads to run");
DEFINE_uint32(duration, 60, "Test duration in seconds");
DEFINE_uint32(
    subscriber_ramp,
    50,
    "Max subscribers per second across all threads");
DEFINE_uint32(subscriber_max, 1000, "Max total subscribers");
DEFINE_uint32(
    first_object_size,
    7576,
    "Size of first object in group (I-frame)");
DEFINE_uint32(
    other_object_size,
    1894,
    "Size of other objects in group (P-frame)");
DEFINE_uint32(delivery_timeout, 500, "Delivery timeout in milliseconds");
DEFINE_uint32(objects_per_group, 30, "Number of objects per group");
DEFINE_uint32(
    object_interval_ms,
    33,
    "Interval between objects in milliseconds");
DEFINE_string(
    metrics_out,
    "",
    "If set, write Prometheus .prom metrics (including the end-to-end latency "
    "histogram) to this path once per second, for a node_exporter textfile "
    "collector to scrape");

namespace {

using Clients = std::vector<std::unique_ptr<moxygen::MoQPerfTestClient>>;

constexpr double kBitsPerMbit = 1024.0 * 1024.0;

struct Totals {
  uint64_t peakSubscribers{0};
  uint64_t currentSubscribers{0};
  uint64_t objects{0};
  uint64_t bytes{0};
  uint64_t resets{0};
  uint64_t failures{0};
  size_t completed{0};
  moxygen::LatencyHistogram latency;
  moxygen::AtomicLatency::Interval interval;

  double avgLatencyMs() const {
    return latency.count() > 0 ? static_cast<double>(latency.sum()) /
            static_cast<double>(latency.count())
                               : 0.0;
  }
};

// Safe from any thread.  Drains each client's interval latency.
Totals sumResults(const Clients& clients) {
  Totals t;
  for (const auto& client : clients) {
    auto r = client->getResults();
    t.peakSubscribers += r.subscribersReached;
    t.currentSubscribers += r.currentSubscribers;
    t.objects += r.totalObjects;
    t.bytes += r.totalBytes;
    t.resets += r.totalResets;
    t.failures += r.totalFailures;
    t.completed += r.trackEnded ? 1 : 0;
    t.latency.merge(r.latency);
    t.interval.merge(r.intervalLatency);
  }
  return t;
}

// Rewrite the whole .prom file each tick.
void writePromFile(
    const std::string& path,
    const std::string& labels,
    const Totals& t,
    double throughputMbps) {
  moxygen::PromWriter w(labels);
  w.gauge("moqperf_subscribers", "Active subscribers", t.currentSubscribers);
  w.gauge(
      "moqperf_throughput_mbps", "Interval throughput in Mbps", throughputMbps);
  w.counter("moqperf_objects_total", "Objects received", t.objects);
  w.counter("moqperf_bytes_total", "Bytes received", t.bytes);
  w.counter("moqperf_resets_total", "Subgroup resets", t.resets);
  w.counter("moqperf_failures_total", "Subscribe failures", t.failures);
  w.gauge(
      "moqperf_latency_avg_ms",
      "Run-average end-to-end object latency in ms",
      t.avgLatencyMs());
  w.histogram(
      "moqperf_object_latency_seconds",
      "End-to-end object latency in seconds",
      {{"", t.latency}});
  w.writeFile(path);
}

folly::coro::Task<void> aggregateStats(
    const Clients& clients,
    folly::CancellationToken cancelToken,
    std::string metricsOut,
    std::string promLabels) {
  auto startTime = std::chrono::steady_clock::now();
  Totals last;

  while (!cancelToken.isCancellationRequested()) {
    co_await folly::coro::sleepReturnEarlyOnCancel(std::chrono::seconds(1));

    auto t = sumResults(clients);
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::steady_clock::now() - startTime)
                       .count();
    double mbps = (t.bytes - last.bytes) * 8.0 / kBitsPerMbit;
    const auto& ivl = t.interval;

    XLOG(INFO) << "[AGGREGATE] [" << elapsed
               << "s] Subs: " << t.currentSubscribers
               << " | Obj/s: " << t.objects - last.objects
               << " | Mbps: " << std::fixed << std::setprecision(2) << mbps
               << " | Total: " << t.objects << " objs, " << std::fixed
               << std::setprecision(2) << t.bytes / (1024.0 * 1024.0) << " MB"
               << " | Latency(run avg): " << std::fixed << std::setprecision(1)
               << t.avgLatencyMs() << " ms"
               << " | Latency(interval min/avg/max): "
               << (ivl.count > 0 ? ivl.minMs : 0) << "/" << std::fixed
               << std::setprecision(1)
               << (ivl.count > 0 ? static_cast<double>(ivl.sumMs) /
                           static_cast<double>(ivl.count)
                                 : 0.0)
               << "/" << (ivl.count > 0 ? ivl.maxMs : 0) << " ms"
               << " | Resets: " << t.resets - last.resets << "/s, " << t.resets
               << " total"
               << " | Failures: " << t.failures - last.failures << "/s, "
               << t.failures << " total"
               << " | Done: " << t.completed << "/" << clients.size();

    if (!metricsOut.empty()) {
      writePromFile(metricsOut, promLabels, t, mbps);
    }
    last = t;

    if (t.completed >= clients.size()) {
      XLOG(INFO) << "[AGGREGATE] All tracks ended - stopping stats aggregation";
      break;
    }
  }
}

} // namespace

int main(int argc, char** argv) {
  gflags::ParseCommandLineFlags(&argc, &argv, false);
  folly::Init init(&argc, &argv);
  auto transportType =
      moxygen::samples::selectClientTransport("transport", "quic_transport");
  const char* transportName =
      transportType == moxygen::samples::TransportType::QMUX   ? "QMUX"
      : transportType == moxygen::samples::TransportType::QUIC ? "QUIC"
                                                               : "WebTransport";

  XLOG(INFO) << "MoQ Performance Test Client (Multi-threaded)";
  XLOG(INFO) << "Relay URL: " << FLAGS_relay_url;
  XLOG(INFO) << "Transport: " << transportName;
  if (FLAGS_versions.empty()) {
    XLOG(INFO) << "MoQ versions: (all supported)";
  } else {
    XLOG(INFO) << "MoQ versions: " << FLAGS_versions;
  }
  XLOG(INFO) << "Number of threads: " << FLAGS_num_threads;
  XLOG(INFO) << "Duration: " << FLAGS_duration << " seconds";
  XLOG(INFO) << "Subscriber ramp (total): " << FLAGS_subscriber_ramp << "/sec";
  XLOG(INFO) << "Subscriber max: " << FLAGS_subscriber_max;
  XLOG(INFO) << "First object size: " << FLAGS_first_object_size << " bytes";
  XLOG(INFO) << "Other object size: " << FLAGS_other_object_size << " bytes";
  XLOG(INFO) << "Delivery timeout: " << FLAGS_delivery_timeout << " ms";

  if (FLAGS_num_threads == 0 || FLAGS_objects_per_group == 0) {
    XLOG(ERR) << "--num_threads and --objects_per_group must be at least 1";
    return 1;
  }

  // Divide the max subscribers per second across all threads
  uint32_t subscriberRampPerThread =
      std::max(1u, FLAGS_subscriber_ramp / FLAGS_num_threads);
  uint32_t subscriberMaxPerThread =
      std::max(1u, FLAGS_subscriber_max / FLAGS_num_threads);

  XLOG(INFO) << "Subscriber ramp (per thread): " << subscriberRampPerThread
             << "/sec; Max subscribers (per thread): "
             << subscriberMaxPerThread;

  std::string versionsLabel = FLAGS_versions.empty() ? "all" : FLAGS_versions;
  std::ostringstream labelStream;
  labelStream << "transport=\"" << moxygen::escapeLabelValue(transportName)
              << "\""
              << ",subs=\"" << FLAGS_subscriber_max << "\""
              << ",first_object_size=\"" << FLAGS_first_object_size << "\""
              << ",other_object_size=\"" << FLAGS_other_object_size << "\""
              << ",versions=\"" << moxygen::escapeLabelValue(versionsLabel)
              << "\"";
  std::string promLabels = labelStream.str();

  try {
    auto url = proxygen::URL(FLAGS_relay_url);
    folly::CancellationSource cancelSource;

    XLOG(INFO) << "Starting " << FLAGS_num_threads << " client thread(s)...";

    auto executor = std::make_unique<folly::IOThreadPoolExecutor>(
        FLAGS_num_threads,
        std::make_shared<folly::NamedThreadFactory>("MoQPerfTest"),
        folly::EventBaseManager::get(),
        folly::IOThreadPoolExecutor::Options().setWaitForAll(true));

    Clients clients;
    uint32_t i = 0;
    for (auto& evb : executor->getAllEventBases()) {
      auto client = std::make_unique<moxygen::MoQPerfTestClient>(
          evb.get(),
          url,
          transportType,
          FLAGS_duration,
          subscriberRampPerThread,
          subscriberMaxPerThread,
          FLAGS_first_object_size,
          FLAGS_other_object_size,
          FLAGS_delivery_timeout,
          FLAGS_objects_per_group,
          FLAGS_object_interval_ms);

      XLOG(INFO) << "Thread " << i++ << " starting...";
      folly::coro::co_withExecutor(evb.get(), client->run()).start();
      clients.push_back(std::move(client));
    }

    {
      folly::ScopedEventBaseThread statsThread;
      folly::coro::co_withExecutor(
          statsThread.getEventBase(),
          aggregateStats(
              clients, cancelSource.getToken(), FLAGS_metrics_out, promLabels))
          .start();

      // Wait for all client tasks to complete
      executor->stop();
      cancelSource.requestCancellation();
    }

    auto t = sumResults(clients);
    auto duration = clients[0]->getResults().durationSeconds;
    double throughputMbps = duration > 0
        ? t.bytes * 8.0 / kBitsPerMbit / static_cast<double>(duration)
        : 0.0;

    XLOG(INFO) << "========================================";
    XLOG(INFO) << "Final Test Summary (All Threads):";
    XLOG(INFO) << "  Threads: " << FLAGS_num_threads;
    XLOG(INFO) << "  Total Subscribers: " << t.peakSubscribers;
    XLOG(INFO) << "  Total Objects: " << t.objects;
    XLOG(INFO) << "  Total Bytes: " << t.bytes;
    XLOG(INFO) << "  Total Resets: " << t.resets;
    XLOG(INFO) << "  Duration: " << duration << " seconds";
    XLOG(INFO) << "  Throughput: " << fmt::format("{:.2f}", throughputMbps)
               << " Mbps";
    if (t.latency.count() > 0) {
      XLOG(INFO) << "  Avg Object Latency: "
                 << fmt::format("{:.1f}", t.avgLatencyMs()) << " ms ("
                 << t.latency.count() << " objects measured)";
    }
    XLOG(INFO) << "  Result: "
               << (t.completed == clients.size()
                       ? "SUCCESS - Track ended naturally"
                       : "Test stopped");

    // Client threads are already joined, so this snapshot captures the final
    // tail.
    if (!FLAGS_metrics_out.empty()) {
      writePromFile(FLAGS_metrics_out, promLabels, t, throughputMbps);
    }
    return 0;
  } catch (const std::exception& ex) {
    XLOG(ERR) << "Exception: " << ex.what();
    return 1;
  }
}
