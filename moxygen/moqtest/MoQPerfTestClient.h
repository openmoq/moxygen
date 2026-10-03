/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>
#include <vector>

#include <folly/container/F14Map.h>
#include <folly/io/async/EventBase.h>
#include <proxygen/lib/utils/URL.h>

#include "moxygen/MoQClientBase.h"
#include "moxygen/ObjectReceiver.h"
#include "moxygen/events/MoQFollyExecutorImpl.h"
#include "moxygen/moqtest/LatencyHistogram.h"
#include "moxygen/moqtest/Types.h"
#include "moxygen/samples/util/Utils.h"

namespace moxygen {

class MoQPerfTestClient;

// Tracks state and statistics for a single subscriber
class SubscriberState {
 public:
  SubscriberState(
      MoQPerfTestClient& testClient,
      size_t id,
      std::shared_ptr<MoQFollyExecutorImpl> executor,
      const proxygen::URL& url,
      samples::TransportType transportType);

  ~SubscriberState();

  // Deleted copy/move to keep pointers stable
  SubscriberState(const SubscriberState&) = delete;
  SubscriberState& operator=(const SubscriberState&) = delete;
  SubscriberState(SubscriberState&&) = delete;
  SubscriberState& operator=(SubscriberState&&) = delete;

  folly::coro::Task<void> connect();
  folly::coro::Task<void> subscribe(
      const MoQTestParameters& params,
      uint32_t deliveryTimeoutMs);

  // Drain the MoQ session
  void drain();

  MoQPerfTestClient& testClient_;
  size_t id_;
  bool hasError_{false};

 private:
  // ObjectReceiverCallback implementation.
  // The MoQ session keeps subgroup receivers (and therefore this callback)
  // alive past SubscriberState destruction, so detach() severs the back
  // pointer and later callbacks become no-ops.
  class Callback : public ObjectReceiverCallback {
   public:
    explicit Callback(SubscriberState& state) : state_(&state) {}

    void detach() {
      state_ = nullptr;
    }

    FlowControlState onObject(
        std::optional<TrackAlias> trackAlias,
        const ObjectHeader& objHeader,
        Payload payload) override;

    void onObjectStatus(
        std::optional<TrackAlias> trackAlias,
        const ObjectHeader& objHeader) override;

    void onEndOfStream() override;
    void onError(ResetStreamErrorCode code) override;
    void onPublishDone(PublishDone done) override;
    void onAllDataReceived() override;

   private:
    SubscriberState* state_;
  };

  std::shared_ptr<Callback> callback_{std::make_shared<Callback>(*this)};
  std::shared_ptr<MoQFollyExecutorImpl> moqExecutor_;
  std::unique_ptr<MoQClientBase> moqClient_;
  std::shared_ptr<ObjectReceiver> receiver_;
  std::shared_ptr<Publisher::SubscriptionHandle> subHandle_;
};

// Main performance test client that manages multiple subscribers
class MoQPerfTestClient {
 public:
  MoQPerfTestClient(
      folly::EventBase* evb,
      proxygen::URL url,
      samples::TransportType transportType,
      uint32_t durationSeconds,
      uint32_t maxSubscribersPerSecond,
      uint32_t maxSubscribers,
      uint32_t firstObjectSize,
      uint32_t otherObjectSize,
      uint32_t deliveryTimeoutMs,
      uint32_t objectsPerGroup,
      uint32_t objectIntervalMs);

  ~MoQPerfTestClient() = default;

  // Run the performance test
  folly::coro::Task<void> run();

  struct TestResults {
    size_t subscribersReached{0}; // peak
    size_t currentSubscribers{0};
    uint64_t totalObjects{0};
    uint64_t totalBytes{0};
    LatencyHistogram latency;
    // Drained by each call, so only the aggregator should ask for it.
    AtomicLatency::Interval intervalLatency;
    uint32_t totalResets{0};
    uint32_t totalFailures{0};
    uint32_t durationSeconds{0};
    bool trackEnded{false};
  };

  // Safe from any thread.
  TestResults getResults() const;

  void completed();
  void recordReset();
  void recordFailure();
  void recordTrackRestart();
  void removeSubscriber(size_t id);
  void updateLargestObjectSeen(const AbsoluteLocation& location);
  void recordObject(uint64_t bytes, std::optional<uint64_t> latencyMs);
  std::optional<AbsoluteLocation> getLargestObjectSeen() const;

 private:
  // Add a new subscriber
  folly::coro::Task<void> addSubscriber();

  // Configuration
  folly::EventBase* evb_;
  proxygen::URL url_;
  samples::TransportType transportType_;
  uint32_t durationSeconds_;
  uint32_t maxSubscribersPerSecond_;
  uint32_t maxSubscribers_;
  uint32_t deliveryTimeoutMs_;

  // Shared executor for all subscribers
  std::shared_ptr<MoQFollyExecutorImpl> sharedExecutor_;

  // MoQ test parameters
  MoQTestParameters params_;

  // Single-threaded state (only accessed on client thread)
  size_t subscribersAdded_{0};
  folly::F14FastMap<size_t, std::unique_ptr<SubscriberState>> subscribers_;
  uint32_t resetsInCurrentInterval_{0};
  uint32_t failuresInCurrentInterval_{0};
  bool trackRestarted_{false};
  std::optional<AbsoluteLocation> largestObjectSeen_;
  std::atomic<std::chrono::steady_clock::time_point> startTime_;

  // Atomic cross-thread state (accessed from aggregation thread in getResults)
  std::atomic<size_t> currentSubscribers_{0};
  std::atomic<size_t> peakSubscribers_{0};
  std::atomic<uint32_t> totalResets_{0};
  std::atomic<uint32_t> totalFailures_{0};
  std::atomic<uint32_t> numCompleted_{0};
  std::atomic<uint64_t> objects_{0};
  std::atomic<uint64_t> bytes_{0};
  mutable AtomicLatency latency_;
};

} // namespace moxygen
