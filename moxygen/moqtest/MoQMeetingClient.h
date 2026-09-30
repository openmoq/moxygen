/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <array>
#include <atomic>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <folly/container/F14Map.h>
#include <folly/coro/Task.h>
#include <folly/io/async/EventBase.h>
#include <proxygen/lib/utils/URL.h>

#include "moxygen/events/MoQFollyExecutorImpl.h"
#include "moxygen/moqtest/LatencyHistogram.h"
#include "moxygen/samples/util/Utils.h"

namespace moxygen {

class Meeting;
class MoQTestPublisher;
class Participant;

// Why a meeting was cancelled.  The first reason reported wins.
enum class MeetingFailure : uint8_t { Reset, Missing, Audio, Session };
constexpr size_t kNumMeetingFailures = 4;
const char* meetingFailureName(MeetingFailure reason);

struct MeetingConfig {
  proxygen::URL url;
  samples::TransportType transportType{samples::TransportType::WEB_TRANSPORT};
  std::string versions;
  uint32_t participants{5};
  uint32_t deliveryTimeoutMs{500};
  uint32_t joinGraceMs{3000};
  uint32_t joinJitterMs{50};
  uint32_t audioLossTolerance{0};
  uint32_t videoFirstObjectSize{7576};
  uint32_t videoObjectSize{1894};
  uint32_t videoObjectsPerGroup{30};
  uint32_t videoObjectIntervalMs{33};
  uint32_t audioObjectSize{160};
  uint32_t audioIntervalMs{20};
  uint32_t connectTimeoutMs{1000};
  uint32_t transactionTimeoutMs{60000};
};

// Receive-side counters for the participants on one EventBase.  Kept per
// EventBase so the receive path doesn't share cache lines across threads.
struct TrafficStats {
  std::atomic<uint64_t> videoObjects{0};
  std::atomic<uint64_t> videoBytes{0};
  std::atomic<uint64_t> audioDatagrams{0};
  std::atomic<uint64_t> audioBytes{0};
  std::atomic<uint64_t> resets{0};
  std::atomic<uint64_t> audioLost{0};
  std::atomic<uint64_t> audioLate{0};
  AtomicLatency videoLatency;
  AtomicLatency audioLatency;
};

// One client EventBase and what the participants placed on it share.
struct MeetingWorker {
  explicit MeetingWorker(folly::EventBase* evb);
  ~MeetingWorker();

  // Cancels and releases every participant still registered.  Run on evb.
  void shutdown();

  folly::EventBase* evb;
  std::shared_ptr<MoQFollyExecutorImpl> executor;
  // Shared by the EventBase's participants, since each owns a timekeeper
  // thread.
  std::shared_ptr<MoQTestPublisher> publisher;
  TrafficStats stats;
  // Touched only on evb, so a participant is always destroyed on its own
  // thread.
  folly::F14FastMap<uint64_t, std::shared_ptr<Participant>> participants;
};

// Ramps up meetings across the workers and backs off when meetings fail.
// Runs on the controller EventBase, the only thread that touches meetings.
class MoQMeetingClient {
 public:
  struct RampConfig {
    uint32_t durationSeconds{60};
    uint32_t meetingRamp{5};
    uint32_t meetingMax{100};
    bool colocate{false};
    std::string meetingPrefix;
  };

  MoQMeetingClient(
      folly::EventBase* controllerEvb,
      std::vector<MeetingWorker*> workers,
      MeetingConfig config,
      RampConfig ramp);
  ~MoQMeetingClient();

  folly::coro::Task<void> run();

  struct Results {
    uint64_t activeMeetings{0};
    uint64_t joinedMeetings{0};
    uint64_t peakMeetings{0};
    uint64_t meetingsStarted{0};
    std::array<uint64_t, kNumMeetingFailures> failures{};
    uint64_t rxTracks{0};
    uint64_t selfEchoTracks{0};
    // Participants whose PUBLISHes and SUBSCRIBE_TRACKS all succeeded.
    uint64_t publishingParticipants{0};
  };
  // Safe from any thread.
  Results getResults() const;

  const MeetingConfig& config() const {
    return config_;
  }
  folly::EventBase* controllerEvb() const {
    return controllerEvb_;
  }

  // Called by Meeting on the controller EventBase.
  void onMeetingFailed(MeetingFailure reason);
  void onMeetingJoined();
  void onParticipantPublishing();
  void onPeerTrack(bool selfEcho);
  void onMeetingGone(
      uint64_t index,
      uint64_t rxTracks,
      bool joined,
      uint32_t publishing);

 private:
  void startMeeting();

  folly::EventBase* controllerEvb_;
  std::vector<MeetingWorker*> workers_;
  MeetingConfig config_;
  RampConfig ramp_;

  uint64_t nextMeeting_{0};
  uint64_t nextParticipantKey_{0};
  folly::F14FastMap<uint64_t, std::shared_ptr<Meeting>> meetings_;
  uint32_t failuresInInterval_{0};

  std::atomic<uint64_t> activeMeetings_{0};
  std::atomic<uint64_t> joinedMeetings_{0};
  std::atomic<uint64_t> peakMeetings_{0};
  std::atomic<uint64_t> meetingsStarted_{0};
  std::array<std::atomic<uint64_t>, kNumMeetingFailures> failures_{};
  std::atomic<uint64_t> rxTracks_{0};
  std::atomic<uint64_t> selfEchoTracks_{0};
  std::atomic<uint64_t> publishingParticipants_{0};
};

} // namespace moxygen
