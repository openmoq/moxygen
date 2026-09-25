/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "moxygen/moqtest/MoQMeetingClient.h"

#include <algorithm>
#include <deque>

#include <folly/Random.h>
#include <folly/coro/Collect.h>
#include <folly/coro/Sleep.h>
#include <folly/logging/xlog.h>

#include "moxygen/MoQVersions.h"
#include "moxygen/ObjectReceiver.h"
#include "moxygen/moqtest/MoQTestPublisher.h"
#include "moxygen/moqtest/Utils.h"
#include "moxygen/util/InsecureVerifierDangerousDoNotUseInProduction.h"

namespace moxygen {

namespace {

constexpr const char* kVideoTrack = "video";
constexpr const char* kAudioTrack = "audio";

uint64_t nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

MoQTestParameters videoParams(const MeetingConfig& cfg) {
  MoQTestParameters params;
  params.forwardingPreference = ForwardingPreference::ONE_SUBGROUP_PER_GROUP;
  params.objectsPerGroup = cfg.videoObjectsPerGroup;
  params.lastObjectInTrack = cfg.videoObjectsPerGroup - 1;
  params.sizeOfObjectZero = cfg.videoFirstObjectSize;
  params.sizeOfObjectGreaterThanZero = cfg.videoObjectSize;
  params.objectFrequency = cfg.videoObjectIntervalMs;
  return params;
}

// One datagram per group, so a gap in group numbers is a lost datagram.
MoQTestParameters audioParams(const MeetingConfig& cfg) {
  MoQTestParameters params;
  params.forwardingPreference = ForwardingPreference::DATAGRAM;
  params.objectsPerGroup = 1;
  params.lastObjectInTrack = 0;
  params.sizeOfObjectZero = cfg.audioObjectSize;
  params.sizeOfObjectGreaterThanZero = cfg.audioObjectSize;
  params.objectFrequency = cfg.audioIntervalMs;
  return params;
}

} // namespace

const char* meetingFailureName(MeetingFailure reason) {
  switch (reason) {
    case MeetingFailure::Reset:
      return "reset";
    case MeetingFailure::Missing:
      return "missing";
    case MeetingFailure::Audio:
      return "audio";
    case MeetingFailure::Session:
      return "session";
  }
  return "unknown";
}

// ============================================================================
// MeetingWorker
// ============================================================================

MeetingWorker::MeetingWorker(folly::EventBase* e)
    : evb(e),
      executor(std::make_shared<MoQFollyExecutorImpl>(e)),
      publisher(std::make_shared<MoQTestPublisher>()) {
  publisher->setIncludeTimestampExtension(true);
}

MeetingWorker::~MeetingWorker() = default;

// ============================================================================
// Meeting: controller-thread bookkeeping for one meeting
// ============================================================================

class Meeting : public std::enable_shared_from_this<Meeting> {
 public:
  Meeting(MoQMeetingClient& client, uint64_t index, std::string id)
      : client_(client), index_(index), id_(std::move(id)) {}

  const std::string& id() const {
    return id_;
  }

  void join(
      const std::vector<MeetingWorker*>& workers,
      uint64_t& nextParticipantKey,
      bool colocate);

  void onSetupDone(uint32_t participant);
  void onPeerTrack(uint32_t participant, bool selfEcho);
  void onFirstVideoObject(uint32_t participant);
  void fail(MeetingFailure reason, const std::string& detail);
  // Cancels without counting a failure, for the end of the run.
  void shutdown();

 private:
  struct Seat {
    MeetingWorker* worker{nullptr};
    uint64_t key{0};
    bool setupDone{false};
    uint32_t peerTracks{0};
    uint32_t peerVideosFlowing{0};
  };

  void checkJoined();
  void cancelParticipants();
  void onParticipantGone();

  MoQMeetingClient& client_;
  uint64_t index_;
  std::string id_;
  std::vector<Seat> seats_;
  uint64_t rxTracks_{0};
  uint32_t publishing_{0};
  uint32_t gone_{0};
  bool cancelling_{false};
  bool joined_{false};
};

// ============================================================================
// Participant: one MoQ session on its worker's EventBase
// ============================================================================

class Participant : public Subscriber,
                    public std::enable_shared_from_this<Participant> {
 public:
  Participant(
      MeetingWorker& worker,
      MoQMeetingClient& client,
      std::weak_ptr<Meeting> meeting,
      const std::string& meetingId,
      uint32_t index)
      : worker_(worker),
        client_(client),
        cfg_(client.config()),
        meeting_(std::move(meeting)),
        meetingId_(meetingId),
        ns_(std::vector<std::string>{meetingId, fmt::format("p{}", index)}),
        index_(index) {}

  void start(std::chrono::milliseconds delay) {
    folly::coro::co_withCancellation(
        cancelSource_.getToken(),
        folly::coro::co_withExecutor(worker_.evb, run(delay)))
        .start();
  }

  void cancel();

  PublishResult publish(
      PublishRequest pub,
      std::shared_ptr<SubscriptionHandle> handle) override;

 private:
  class RxCallback;

  folly::coro::Task<void> run(std::chrono::milliseconds delay);
  folly::coro::Task<void> stream(
      folly::coro::Task<void> task,
      const char* kind);

  // Runs fn on the controller EventBase, if the meeting is still around.
  template <typename Fn>
  void postToMeeting(Fn&& fn) {
    client_.controllerEvb()->runInEventBaseThread(
        [meeting = meeting_, fn = std::forward<Fn>(fn)]() mutable {
          if (auto m = meeting.lock()) {
            fn(*m);
          }
        });
  }

  void fail(MeetingFailure reason, std::string detail);
  void detachSession();

  MeetingWorker& worker_;
  MoQMeetingClient& client_;
  const MeetingConfig& cfg_;
  std::weak_ptr<Meeting> meeting_;
  std::string meetingId_;
  TrackNamespace ns_;
  uint32_t index_;
  std::unique_ptr<MoQClientBase> moqClient_;
  std::shared_ptr<Publisher::SubscribeTracksHandle> subTracksHandle_;
  struct RxTrack {
    std::shared_ptr<RxCallback> callback;
    std::shared_ptr<SubscriptionHandle> handle;
  };
  std::vector<RxTrack> rxTracks_;
  folly::CancellationSource cancelSource_;
  bool failed_{false};
  bool cancelled_{false};
};

// The session keeps receivers (and so this callback) alive past the
// participant, so detach() severs the back pointer and later callbacks become
// no-ops.
class Participant::RxCallback : public ObjectReceiverCallback {
 public:
  RxCallback(Participant& participant, bool video, bool selfEcho)
      : participant_(&participant), video_(video), selfEcho_(selfEcho) {}

  void detach() {
    participant_ = nullptr;
  }

  FlowControlState onObject(
      std::optional<TrackAlias> /*trackAlias*/,
      const ObjectHeader& header,
      Payload payload) override;

  void onObjectStatus(
      std::optional<TrackAlias> /*trackAlias*/,
      const ObjectHeader& /*header*/) override {}

  void onEndOfStream() override {}

  void onError(ResetStreamErrorCode code) override {
    if (!participant_) {
      return;
    }
    participant_->worker_.stats.resets.fetch_add(1, std::memory_order_relaxed);
    participant_->fail(
        MeetingFailure::Reset,
        fmt::format(
            "{} stream reset, code {}",
            video_ ? kVideoTrack : kAudioTrack,
            folly::to_underlying(code)));
  }

  void onPublishDone(PublishDone done) override {
    if (!participant_) {
      return;
    }
    participant_->fail(
        MeetingFailure::Session,
        fmt::format(
            "PUBLISH_DONE on {} track, status {}: {}",
            video_ ? kVideoTrack : kAudioTrack,
            folly::to_underlying(done.statusCode),
            done.reasonPhrase));
  }

 private:
  // Counts a gap as lost only once the track has moved this many groups past
  // it, so a datagram that arrives out of order doesn't count.
  static constexpr uint64_t kReorderWindow = 3;

  uint64_t trackAudioGroup(uint64_t group);

  Participant* participant_;
  bool video_;
  bool selfEcho_;
  bool sawObject_{false};
  std::optional<uint64_t> highestAudioGroup_;
  std::deque<uint64_t> missingAudioGroups_;
  uint64_t badAudio_{0};
};

// Returns how many datagrams are newly known to be lost.  Losses are counted
// from the first datagram, since a subscriber joins the track in progress.
uint64_t Participant::RxCallback::trackAudioGroup(uint64_t group) {
  uint64_t lost = 0;
  if (!highestAudioGroup_) {
    highestAudioGroup_ = group;
  } else if (group > *highestAudioGroup_) {
    auto first = *highestAudioGroup_ + 1;
    if (group - first > kReorderWindow) {
      lost += group - kReorderWindow - first;
      first = group - kReorderWindow;
    }
    for (auto g = first; g < group; ++g) {
      missingAudioGroups_.push_back(g);
    }
    highestAudioGroup_ = group;
  } else {
    auto it = std::find(
        missingAudioGroups_.begin(), missingAudioGroups_.end(), group);
    if (it != missingAudioGroups_.end()) {
      missingAudioGroups_.erase(it);
    }
  }
  while (!missingAudioGroups_.empty() &&
         *highestAudioGroup_ - missingAudioGroups_.front() > kReorderWindow) {
    missingAudioGroups_.pop_front();
    ++lost;
  }
  return lost;
}

ObjectReceiverCallback::FlowControlState Participant::RxCallback::onObject(
    std::optional<TrackAlias> /*trackAlias*/,
    const ObjectHeader& header,
    Payload payload) {
  if (!participant_) {
    return FlowControlState::UNBLOCKED;
  }
  auto& stats = participant_->worker_.stats;
  const auto& cfg = participant_->cfg_;
  uint64_t bytes = payload ? payload->computeChainDataLength() : 0;
  std::optional<uint64_t> latencyMs;
  if (auto sendTs =
          header.extensions.getIntExtension(kTimestampExtensionType)) {
    auto now = nowMs();
    if (now >= *sendTs) {
      latencyMs = now - *sendTs;
    }
  }

  if (video_) {
    stats.videoObjects.fetch_add(1, std::memory_order_relaxed);
    stats.videoBytes.fetch_add(bytes, std::memory_order_relaxed);
    if (latencyMs) {
      stats.videoLatency.record(*latencyMs);
    }
    if (!sawObject_) {
      sawObject_ = true;
      if (!selfEcho_) {
        participant_->postToMeeting([i = participant_->index_](Meeting& m) {
          m.onFirstVideoObject(i);
        });
      }
    }
    return FlowControlState::UNBLOCKED;
  }

  stats.audioDatagrams.fetch_add(1, std::memory_order_relaxed);
  stats.audioBytes.fetch_add(bytes, std::memory_order_relaxed);
  if (latencyMs) {
    stats.audioLatency.record(*latencyMs);
  }
  uint64_t lost = trackAudioGroup(header.group);
  bool late = false;
  if (lost > 0) {
    stats.audioLost.fetch_add(lost, std::memory_order_relaxed);
  }
  if (latencyMs && cfg.deliveryTimeoutMs > 0 &&
      *latencyMs > cfg.deliveryTimeoutMs) {
    late = true;
    stats.audioLate.fetch_add(1, std::memory_order_relaxed);
  }
  badAudio_ += lost + (late ? 1 : 0);
  if ((lost > 0 || late) && badAudio_ > cfg.audioLossTolerance) {
    participant_->fail(
        MeetingFailure::Audio,
        fmt::format(
            "audio group {}: {} lost, latency {} ms ({} bad total)",
            header.group,
            lost,
            latencyMs.value_or(0),
            badAudio_));
  }
  return FlowControlState::UNBLOCKED;
}

folly::coro::Task<void> Participant::run(std::chrono::milliseconds delay) {
  auto self = shared_from_this();
  std::optional<folly::coro::Task<void>> videoTask;
  std::optional<folly::coro::Task<void>> audioTask;
  std::optional<std::string> setupError;
  try {
    if (delay.count() > 0) {
      co_await folly::coro::sleep(delay);
    }
    moqClient_ = samples::makeRelayClientTransport(
        worker_.executor,
        cfg_.url,
        std::make_shared<test::InsecureVerifierDangerousDoNotUseInProduction>(),
        cfg_.transportType);
    co_await moqClient_->setupMoQSession(
        std::chrono::milliseconds(cfg_.connectTimeoutMs),
        std::chrono::milliseconds(cfg_.transactionTimeoutMs),
        /*publishHandler=*/nullptr,
        /*subscribeHandler=*/self,
        [] {
          quic::TransportSettings ts;
          ts.orderedReadCallbacks = true;
          ts.rxPacketsBeforeAckAfterInit = 2;
          ts.shouldUseRecvmmsgForBatchRecv = true;
          ts.maxRecvBatchSize = 32;
          return ts;
        }(),
        getMoqtProtocols(cfg_.versions, true));
    auto session = moqClient_->moqSession_;

    videoTask = co_await worker_.publisher->startPublishTrack(
        session,
        FullTrackName{ns_, kVideoTrack},
        videoParams(cfg_),
        RequestID(0));
    audioTask = co_await worker_.publisher->startPublishTrack(
        session,
        FullTrackName{ns_, kAudioTrack},
        audioParams(cfg_),
        RequestID(1));

    SubscribeTracks subTracks;
    subTracks.trackNamespacePrefix =
        TrackNamespace(std::vector<std::string>{meetingId_});
    subTracks.forward = true;
    auto res = co_await session->subscribeTracks(std::move(subTracks));
    if (res.hasError()) {
      throw std::runtime_error(fmt::format(
          "SUBSCRIBE_TRACKS failed, code {}: {}",
          folly::to_underlying(res.error().errorCode),
          res.error().reasonPhrase));
    }
    subTracksHandle_ = std::move(res.value());
  } catch (const folly::OperationCancelled&) {
    setupError = "cancelled";
  } catch (const std::exception& ex) {
    setupError = ex.what();
  }
  if (setupError || cancelled_) {
    // An unstarted stream task still holds the publisher's unpause
    // registration.  Running it under a cancelled token releases that.
    folly::CancellationSource cancelled;
    cancelled.requestCancellation();
    for (auto* task : {&videoTask, &audioTask}) {
      if (*task) {
        co_await folly::coro::co_awaitTry(folly::coro::co_withCancellation(
            cancelled.getToken(), std::move(**task)));
      }
    }
    // cancel() may have run before setup created what it tears down.
    detachSession();
    if (setupError) {
      fail(MeetingFailure::Session, *setupError);
    }
    co_return;
  }
  postToMeeting([i = index_](Meeting& m) { m.onSetupDone(i); });
  co_await folly::coro::collectAll(
      stream(std::move(*videoTask), kVideoTrack),
      stream(std::move(*audioTask), kAudioTrack));
}

// The tracks run until cancelled, so ending on their own means the session
// broke underneath them.
folly::coro::Task<void> Participant::stream(
    folly::coro::Task<void> task,
    const char* kind) {
  try {
    co_await std::move(task);
  } catch (const folly::OperationCancelled&) {
    co_return;
  } catch (const std::exception& ex) {
    fail(
        MeetingFailure::Session,
        fmt::format("{} publish failed: {}", kind, ex.what()));
    co_return;
  }
  if (!cancelled_) {
    fail(MeetingFailure::Session, fmt::format("{} publish ended", kind));
  }
}

Subscriber::PublishResult Participant::publish(
    PublishRequest pub,
    std::shared_ptr<SubscriptionHandle> handle) {
  const auto& ftn = pub.fullTrackName;
  const bool video = ftn.trackName == kVideoTrack;
  if (cancelled_ || (!video && ftn.trackName != kAudioTrack)) {
    return folly::makeUnexpected(PublishError{
        pub.requestID, PublishErrorCode::NOT_SUPPORTED, "not a meeting"});
  }
  const bool selfEcho = ftn.trackNamespace == ns_;
  if (selfEcho) {
    XLOG_EVERY_MS(WARN, 10000)
        << "Relay echoed a participant's own track back to it: " << ftn;
  }
  auto callback = std::make_shared<RxCallback>(*this, video, selfEcho);
  auto receiver =
      std::make_shared<ObjectReceiver>(ObjectReceiver::SUBSCRIBE, callback);
  rxTracks_.push_back({callback, std::move(handle)});
  postToMeeting(
      [i = index_, selfEcho](Meeting& m) { m.onPeerTrack(i, selfEcho); });

  PublishOk ok;
  ok.requestID = pub.requestID;
  ok.forward = true;
  ok.subscriberPriority = kDefaultPriority;
  ok.groupOrder = GroupOrder::OldestFirst;
  ok.locType = LocationType::LargestObject;
  if (cfg_.deliveryTimeoutMs > 0) {
    ok.params.insertParam(
        {folly::to_underlying(TrackRequestParamKey::DELIVERY_TIMEOUT),
         static_cast<uint64_t>(cfg_.deliveryTimeoutMs)});
  }
  return PublishConsumerAndReplyTask{
      std::move(receiver),
      folly::coro::makeTask(
          folly::Expected<PublishOk, PublishError>(std::move(ok))),
      /*consumerReady=*/true};
}

void Participant::fail(MeetingFailure reason, std::string detail) {
  if (failed_ || cancelled_) {
    return;
  }
  failed_ = true;
  postToMeeting([reason, detail = fmt::format("p{}: {}", index_, detail)](
                    Meeting& m) { m.fail(reason, detail); });
}

void Participant::cancel() {
  if (cancelled_) {
    return;
  }
  cancelled_ = true;
  cancelSource_.requestCancellation();
  for (auto& track : rxTracks_) {
    track.callback->detach();
  }
  rxTracks_.clear();
  detachSession();
}

void Participant::detachSession() {
  if (subTracksHandle_) {
    subTracksHandle_->unsubscribeTracks();
    subTracksHandle_.reset();
  }
  // The session holds this participant as its subscribe handler.  Clearing it
  // lets the session close when the participant is destroyed.
  if (moqClient_ && moqClient_->moqSession_) {
    moqClient_->moqSession_->setSubscribeHandler(nullptr);
  }
}

void MeetingWorker::shutdown() {
  auto remaining = std::move(participants);
  participants.clear();
  for (auto& [key, participant] : remaining) {
    participant->cancel();
  }
  publisher->cancelAll();
}

// ============================================================================
// Meeting implementation
// ============================================================================

void Meeting::join(
    const std::vector<MeetingWorker*>& workers,
    uint64_t& nextParticipantKey,
    bool colocate) {
  const auto& cfg = client_.config();
  seats_.resize(cfg.participants);
  for (uint32_t i = 0; i < cfg.participants; ++i) {
    auto& seat = seats_[i];
    seat.worker = workers[(colocate ? index_ : index_ + i) % workers.size()];
    seat.key = nextParticipantKey++;
    auto delay = std::chrono::milliseconds(
        cfg.joinJitterMs > 0 ? folly::Random::rand32(cfg.joinJitterMs + 1) : 0);
    seat.worker->evb->runInEventBaseThread([worker = seat.worker,
                                            &client = client_,
                                            meeting = weak_from_this(),
                                            id = id_,
                                            i,
                                            key = seat.key,
                                            delay] {
      auto participant =
          std::make_shared<Participant>(*worker, client, meeting, id, i);
      worker->participants.emplace(key, participant);
      participant->start(delay);
    });
  }
  client_.controllerEvb()->runAfterDelay(
      [meeting = weak_from_this()] {
        if (auto m = meeting.lock()) {
          m->checkJoined();
        }
      },
      cfg.joinGraceMs);
}

void Meeting::onSetupDone(uint32_t participant) {
  if (cancelling_ || seats_[participant].setupDone) {
    return;
  }
  seats_[participant].setupDone = true;
  ++publishing_;
  client_.onParticipantPublishing();
}

void Meeting::onPeerTrack(uint32_t participant, bool selfEcho) {
  client_.onPeerTrack(selfEcho);
  if (selfEcho) {
    return;
  }
  ++rxTracks_;
  ++seats_[participant].peerTracks;
}

void Meeting::onFirstVideoObject(uint32_t participant) {
  ++seats_[participant].peerVideosFlowing;
}

void Meeting::checkJoined() {
  if (cancelling_) {
    return;
  }
  const uint32_t peers = static_cast<uint32_t>(seats_.size()) - 1;
  for (size_t i = 0; i < seats_.size(); ++i) {
    const auto& seat = seats_[i];
    if (!seat.setupDone || seat.peerTracks < 2 * peers ||
        seat.peerVideosFlowing < peers) {
      fail(
          MeetingFailure::Missing,
          fmt::format(
              "p{} after {} ms: setup {}, peer tracks {}/{}, video flowing "
              "{}/{}",
              i,
              client_.config().joinGraceMs,
              seat.setupDone ? "done" : "pending",
              seat.peerTracks,
              2 * peers,
              seat.peerVideosFlowing,
              peers));
      return;
    }
  }
  joined_ = true;
  client_.onMeetingJoined();
}

void Meeting::fail(MeetingFailure reason, const std::string& detail) {
  if (cancelling_) {
    return;
  }
  XLOG(INFO) << "Meeting " << id_ << " cancelled ("
             << meetingFailureName(reason) << "): " << detail;
  client_.onMeetingFailed(reason);
  cancelParticipants();
}

void Meeting::shutdown() {
  if (!cancelling_) {
    cancelParticipants();
  }
}

void Meeting::cancelParticipants() {
  cancelling_ = true;
  for (const auto& seat : seats_) {
    // Posted after the participant's creation to the same EventBase, so the
    // participant is always in the registry by the time this runs.
    seat.worker->evb->runInEventBaseThread(
        [worker = seat.worker,
         key = seat.key,
         controllerEvb = client_.controllerEvb(),
         meeting = weak_from_this()] {
          auto it = worker->participants.find(key);
          if (it != worker->participants.end()) {
            auto participant = std::move(it->second);
            worker->participants.erase(it);
            participant->cancel();
          }
          controllerEvb->runInEventBaseThread([meeting] {
            if (auto m = meeting.lock()) {
              m->onParticipantGone();
            }
          });
        });
  }
}

void Meeting::onParticipantGone() {
  if (++gone_ == seats_.size()) {
    client_.onMeetingGone(index_, rxTracks_, joined_, publishing_);
  }
}

// ============================================================================
// MoQMeetingClient
// ============================================================================

MoQMeetingClient::MoQMeetingClient(
    folly::EventBase* controllerEvb,
    std::vector<MeetingWorker*> workers,
    MeetingConfig config,
    RampConfig ramp)
    : controllerEvb_(controllerEvb),
      workers_(std::move(workers)),
      config_(std::move(config)),
      ramp_(std::move(ramp)) {}

MoQMeetingClient::~MoQMeetingClient() = default;

folly::coro::Task<void> MoQMeetingClient::run() {
  auto startTime = std::chrono::steady_clock::now();
  auto deadline = startTime + std::chrono::seconds(ramp_.durationSeconds);
  auto observe = std::max(
      std::chrono::milliseconds(2000),
      std::chrono::milliseconds(config_.joinGraceMs + 500));

  auto currentIncrement = ramp_.meetingRamp;
  auto token = co_await folly::coro::co_current_cancellation_token;
  auto shouldContinue = [&] {
    return !token.isCancellationRequested() &&
        std::chrono::steady_clock::now() < deadline;
  };

  while (shouldContinue()) {
    failuresInInterval_ = 0;

    uint32_t toAdd = std::min(currentIncrement, ramp_.meetingRamp);
    if (meetings_.size() >= ramp_.meetingMax) {
      toAdd = 0;
    } else {
      toAdd = std::min(
          toAdd, static_cast<uint32_t>(ramp_.meetingMax - meetings_.size()));
    }
    uint32_t sleepMs = toAdd > 0 ? 1000 / toAdd : 0;
    uint32_t added = 0;
    for (; shouldContinue() && added < toAdd; ++added) {
      startMeeting();
      if (sleepMs > 0) {
        co_await folly::coro::sleepReturnEarlyOnCancel(
            std::chrono::milliseconds(sleepMs));
      }
    }
    if (added > 0) {
      XLOG(INFO) << "Added " << added << " meetings (increment "
                 << currentIncrement << ", " << meetings_.size() << " active)";
    }

    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    if (remaining.count() > 0) {
      co_await folly::coro::sleepReturnEarlyOnCancel(
          std::min(observe, remaining));
    }

    if (failuresInInterval_ > 0) {
      currentIncrement = std::max(1u, currentIncrement / 2);
      XLOG(INFO) << failuresInInterval_
                 << " meetings failed during interval - reducing increment to "
                 << currentIncrement;
    } else if (currentIncrement < ramp_.meetingRamp) {
      currentIncrement = std::min(ramp_.meetingRamp, currentIncrement * 2);
      XLOG(INFO) << "No meeting failures - increasing increment to "
                 << currentIncrement;
    }
  }

  XLOG(INFO) << (token.isCancellationRequested() ? "Test cancelled"
                                                 : "Duration reached")
             << " - ending " << meetings_.size() << " meetings";
  auto meetings = meetings_;
  for (auto& [index, meeting] : meetings) {
    meeting->shutdown();
  }
  auto giveUp = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!meetings_.empty() && std::chrono::steady_clock::now() < giveUp) {
    co_await folly::coro::sleep(std::chrono::milliseconds(50));
  }
  if (!meetings_.empty()) {
    XLOG(WARN) << meetings_.size() << " meetings did not finish ending";
    // Destroyed here, on the controller, where their callbacks run.
    meetings_.clear();
  }
}

void MoQMeetingClient::startMeeting() {
  auto index = nextMeeting_++;
  auto meeting = std::make_shared<Meeting>(
      *this, index, fmt::format("{}_m{}", ramp_.meetingPrefix, index));
  meetings_.emplace(index, meeting);
  meetingsStarted_.fetch_add(1, std::memory_order_relaxed);
  activeMeetings_.store(meetings_.size(), std::memory_order_relaxed);
  if (meetings_.size() > peakMeetings_.load(std::memory_order_relaxed)) {
    peakMeetings_.store(meetings_.size(), std::memory_order_relaxed);
  }
  XLOG(DBG1) << "Starting meeting " << meeting->id();
  meeting->join(workers_, nextParticipantKey_, ramp_.colocate);
}

void MoQMeetingClient::onMeetingFailed(MeetingFailure reason) {
  failures_[static_cast<size_t>(reason)].fetch_add(
      1, std::memory_order_relaxed);
  ++failuresInInterval_;
}

void MoQMeetingClient::onMeetingJoined() {
  joinedMeetings_.fetch_add(1, std::memory_order_relaxed);
}

void MoQMeetingClient::onParticipantPublishing() {
  publishingParticipants_.fetch_add(1, std::memory_order_relaxed);
}

void MoQMeetingClient::onPeerTrack(bool selfEcho) {
  (selfEcho ? selfEchoTracks_ : rxTracks_)
      .fetch_add(1, std::memory_order_relaxed);
}

void MoQMeetingClient::onMeetingGone(
    uint64_t index,
    uint64_t rxTracks,
    bool joined,
    uint32_t publishing) {
  meetings_.erase(index);
  activeMeetings_.store(meetings_.size(), std::memory_order_relaxed);
  rxTracks_.fetch_sub(rxTracks, std::memory_order_relaxed);
  publishingParticipants_.fetch_sub(publishing, std::memory_order_relaxed);
  if (joined) {
    joinedMeetings_.fetch_sub(1, std::memory_order_relaxed);
  }
}

MoQMeetingClient::Results MoQMeetingClient::getResults() const {
  Results r;
  r.activeMeetings = activeMeetings_.load(std::memory_order_relaxed);
  r.joinedMeetings = joinedMeetings_.load(std::memory_order_relaxed);
  r.peakMeetings = peakMeetings_.load(std::memory_order_relaxed);
  r.meetingsStarted = meetingsStarted_.load(std::memory_order_relaxed);
  for (size_t i = 0; i < kNumMeetingFailures; ++i) {
    r.failures[i] = failures_[i].load(std::memory_order_relaxed);
  }
  r.rxTracks = rxTracks_.load(std::memory_order_relaxed);
  r.selfEchoTracks = selfEchoTracks_.load(std::memory_order_relaxed);
  r.publishingParticipants =
      publishingParticipants_.load(std::memory_order_relaxed);
  return r;
}

} // namespace moxygen
