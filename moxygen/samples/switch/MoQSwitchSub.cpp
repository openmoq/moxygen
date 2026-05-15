/*
 * Copyright (c) Synamedia
 * SPDX-License-Identifier: Apache-2.0
 */

#include <atomic>
#include <folly/coro/BlockingWait.h>
#include <folly/coro/Sleep.h>
#include <folly/coro/UnboundedQueue.h>
#include <folly/init/Init.h>
#include <folly/io/async/EventBaseManager.h>
#include <folly/io/async/ScopedEventBaseThread.h>
#include <folly/json.h>
#include <folly/portability/GFlags.h>
#include <moxygen/MoQConsumers.h>
#include <moxygen/MoQWebTransportClient.h>
#include <moxygen/events/MoQFollyExecutorImpl.h>
#include <moxygen/relay/MoQRelayClient.h>
#include <moxygen/util/InsecureVerifierDangerousDoNotUseInProduction.h>
#include <quic/folly_utils/Utils.h>
#include "switch/SwitchTypes.h"

DEFINE_string(
    relay_url,
    "https://localhost:4433/moq-relay",
    "Relay WebTransport URL");
DEFINE_string(ns, "test", "Track namespace");
DEFINE_int32(warm_up_groups, 5, "Groups to collect from test/high before SWITCH");
DEFINE_int32(lag_seconds, 0, "Seconds to pause consuming before sending SWITCH");
DEFINE_int32(
    collect_groups,
    10,
    "Live groups to collect from test/low after switch");
DEFINE_bool(insecure, false, "Skip TLS certificate validation");

namespace {
using namespace moxygen;

// POD struct — no std::string — so std::optional<ReceivedObject> and
// std::vector<ReceivedObject> are trivially moveable, avoiding the GCC 11/12
// ICE in morph_fn_to_coro / build_special_member_call that fires when a
// coroutine frame holds non-trivially-moveable types.
struct ReceivedObject {
  bool isLow{false}; // false = "high" track, true = "low" track
  uint64_t group{0};
  uint64_t objectID{0};
  bool catchup{false};
};

using ObjQueue =
    folly::coro::UnboundedQueue<std::optional<ReceivedObject>, true, true>;

class CollectingSubgroup : public SubgroupConsumer {
 public:
  CollectingSubgroup(bool isLow, uint64_t groupID, bool catchup, ObjQueue& q)
      : isLow_(isLow), groupID_(groupID), catchup_(catchup), queue_(q) {}

  folly::Expected<folly::Unit, MoQPublishError> object(
      uint64_t objectID,
      Payload,
      Extensions,
      bool) override {
    queue_.enqueue(ReceivedObject{isLow_, groupID_, objectID, catchup_});
    return folly::unit;
  }
  folly::Expected<folly::Unit, MoQPublishError> beginObject(
      uint64_t,
      uint64_t,
      Payload,
      Extensions) override {
    return folly::unit;
  }
  folly::Expected<ObjectPublishStatus, MoQPublishError> objectPayload(
      Payload,
      bool) override {
    return ObjectPublishStatus::DONE;
  }
  folly::Expected<folly::Unit, MoQPublishError> endOfGroup(uint64_t) override {
    return folly::unit;
  }
  folly::Expected<folly::Unit, MoQPublishError> endOfTrackAndGroup(
      uint64_t) override {
    return folly::unit;
  }
  folly::Expected<folly::Unit, MoQPublishError> endOfSubgroup() override {
    return folly::unit;
  }
  void reset(ResetStreamErrorCode) override {}

 private:
  bool isLow_;
  uint64_t groupID_;
  bool catchup_;
  ObjQueue& queue_;
};

class CollectingConsumer : public TrackConsumer {
 public:
  explicit CollectingConsumer(bool isLow, bool catchup = false)
      : isLow_(isLow), catchup_(catchup) {}

  void setCatchup(bool c) {
    catchup_.store(c, std::memory_order_relaxed);
  }

  folly::Expected<folly::Unit, MoQPublishError> setTrackAlias(
      TrackAlias) override {
    return folly::unit;
  }
  folly::Expected<std::shared_ptr<SubgroupConsumer>, MoQPublishError>
  beginSubgroup(uint64_t groupID, uint64_t, Priority, bool) override {
    return std::make_shared<CollectingSubgroup>(
        isLow_, groupID, catchup_.load(std::memory_order_relaxed), queue_);
  }
  folly::Expected<folly::SemiFuture<folly::Unit>, MoQPublishError>
  awaitStreamCredit() override {
    return folly::makeSemiFuture(folly::unit);
  }
  folly::Expected<folly::Unit, MoQPublishError> objectStream(
      const ObjectHeader& h,
      Payload,
      bool) override {
    queue_.enqueue(ReceivedObject{isLow_, h.group, h.id, catchup_});
    return folly::unit;
  }
  folly::Expected<folly::Unit, MoQPublishError> datagram(
      const ObjectHeader& h,
      Payload,
      bool) override {
    queue_.enqueue(ReceivedObject{isLow_, h.group, h.id, catchup_});
    return folly::unit;
  }
  folly::Expected<folly::Unit, MoQPublishError> publishDone(
      PublishDone) override {
    queue_.enqueue(std::nullopt);
    return folly::unit;
  }

  folly::coro::Task<std::optional<ReceivedObject>> dequeue() {
    co_return co_await queue_.dequeue();
  }

 private:
  bool isLow_;
  std::atomic<bool> catchup_;
  ObjQueue queue_;
};

std::pair<uint64_t, uint64_t> decodeSwitchTransition(const std::string& s) {
  auto buf = folly::IOBuf::wrapBuffer(s.data(), s.size());
  folly::io::Cursor cursor(buf.get());
  auto g = quic::follyutils::decodeQuicInteger(cursor);
  auto l = quic::follyutils::decodeQuicInteger(cursor);
  if (!g || !l) {
    XLOG(ERR) << "decodeSwitchTransition: truncated VARINT in SWITCH_TRANSITION param";
    return {UINT64_MAX, 0};
  }
  return {g->first, l->first};
}

class SwitchSubscriber : public Subscriber,
                         public std::enable_shared_from_this<SwitchSubscriber> {
 public:
  SwitchSubscriber() = default;

  PublishResult publish(
      PublishRequest pub,
      std::shared_ptr<SubscriptionHandle> /*handle*/) override {
    for (const auto& param : pub.params) {
      if (param.key == openmoq::moqx::kSwitchTransitionParamKey) {
        auto [g, l] = decodeSwitchTransition(param.asString);
        switchingGroupID_ = g;
        liveEdgeGroupID_ = l;
        break;
      }
    }
    printEvent(folly::dynamic::object("event", "switch")(
        "g_switch", switchingGroupID_)("live_edge", liveEdgeGroupID_));

    lowConsumer_ = std::make_shared<CollectingConsumer>(/*isLow=*/true, /*catchup=*/true);
    relayClient_->getSession()->registerSwitchFetchConsumer(
        highRequestID_, lowConsumer_);
    if (!publishPromise_.isFulfilled()) {
      publishPromise_.setValue(folly::unit);
    } else {
      XLOG(ERR) << "publish() called more than once — ignoring duplicate relay PUBLISH";
    }

    auto consumer = lowConsumer_;
    auto requestID = pub.requestID;
    return PublishConsumerAndReplyTask{
        consumer,
        [requestID]() -> folly::coro::Task<
            folly::Expected<PublishOk, PublishError>> {
          co_return PublishOk{requestID};
        }()};
  }

  // Non-coroutine: keeps proxygen::URL off every coroutine frame.
  void setupRelayClient() {
    proxygen::URL url(FLAGS_relay_url);
    auto executor = std::make_shared<MoQFollyExecutorImpl>(
        folly::EventBaseManager::get()->getEventBase());
    std::shared_ptr<fizz::CertificateVerifier> verifier;
    if (FLAGS_insecure) {
      verifier = std::make_shared<
          test::InsecureVerifierDangerousDoNotUseInProduction>();
    }
    relayClient_ = std::make_shared<MoQRelayClient>(
        std::make_unique<MoQWebTransportClient>(executor, url, verifier));
  }

  folly::coro::Task<bool> warmup() {
    highConsumer_ = std::make_shared<CollectingConsumer>(/*isLow=*/false);
    auto* evb = folly::EventBaseManager::get()->getEventBase();
    // GCC 12 ICEs whenever SubscribeResult = Expected<shared_ptr<SubscriptionHandle>,
    // RequestError> (which contains std::string) appears in any awaitable stored in a
    // coroutine frame, even via Future<T> wrappers. Bridge via Promise<bool> so the
    // coroutine frame only ever sees Future<bool> — which compiles fine.
    folly::Promise<bool> subscribePromise;
    auto subscribeDone = subscribePromise.getFuture();
    relayClient_->getSession()
        ->subscribe(
            SubscribeRequest::make(
                FullTrackName{TrackNamespace({FLAGS_ns}), "high"}),
            highConsumer_)
        .semi()
        .via(evb)
        .thenValue(
            [this, p = std::move(subscribePromise)](auto res) mutable {
              if (res.hasError()) {
                XLOG(ERR) << "subscribe(high) failed";
                p.setValue(false);
                return;
              }
              highRequestID_ = res.value()->subscribeOk().requestID;
              p.setValue(true);
            });
    if (!co_await std::move(subscribeDone).via(evb)) {
      co_return false;
    }

    // When testing with lag, subscribe to the target "low" track immediately so
    // the relay will REQUEST_UPDATE the publisher and cache "low" groups during
    // the entire warm-up + lag period.  The handle is stored as a member to
    // keep it alive across co_awaits without landing in the coroutine frame
    // (GCC 12 ICE guard).  We unsubscribe before sending SWITCH so the relay
    // PUBLISH from the SWITCH mechanism is the sole delivery path.
    if (FLAGS_lag_seconds > 0) {
      folly::Promise<bool> primePromise;
      auto primeDone = primePromise.getFuture();
      relayClient_->getSession()
          ->subscribe(
              SubscribeRequest::make(
                  FullTrackName{TrackNamespace({FLAGS_ns}), "low"}),
              std::make_shared<CollectingConsumer>(/*isLow=*/true))
          .semi()
          .via(evb)
          .thenValue(
              [this, p = std::move(primePromise)](auto res) mutable {
                if (!res.hasError()) {
                  lowPrimeHandle_ = res.value();
                }
                p.setValue(!res.hasError());
              });
      co_await std::move(primeDone).via(evb);
      // Continue even if prime fails — catch-up simply won't be available.
    }

    for (int i = 0; i < FLAGS_warm_up_groups; ++i) {
      auto obj = co_await highConsumer_->dequeue();
      if (!obj) {
        XLOG(ERR) << "high track ended during warm-up";
        co_return false;
      }
      lastHighGroup_ = obj->group;
      printEvent(folly::dynamic::object("event", "object")("track", "high")(
          "group", obj->group)("object", obj->objectID));
    }

    if (FLAGS_lag_seconds > 0) {
      printEvent(folly::dynamic::object("event", "lag_start")(
          "after_group", lastHighGroup_));
      co_await folly::coro::sleep(std::chrono::seconds(FLAGS_lag_seconds));
      // Unsubscribe "low" before SWITCH so the relay PUBLISH is the sole path.
      if (lowPrimeHandle_) {
        lowPrimeHandle_->unsubscribe();
        lowPrimeHandle_.reset();
      }
      printEvent(folly::dynamic::object("event", "lag_end")(
          "elapsed_ms", FLAGS_lag_seconds * 1000));
    }

    co_return true;
  }

  folly::coro::Task<bool> run() noexcept {
    setupRelayClient();

    co_await relayClient_->setup(
        /*publisher=*/nullptr,
        shared_from_this(),
        std::chrono::milliseconds(2000),
        std::chrono::seconds(120));

    if (!co_await warmup()) {
      co_return false;
    }

    relayClient_->getSession()->sendSwitch(Switch{
        .currentSubscribeRequestID = highRequestID_,
        .targetTrackName =
            FullTrackName{TrackNamespace({FLAGS_ns}), "low"},
        .minimumSwitchingGroupID = lastHighGroup_ + 1});

    auto exec = co_await folly::coro::co_current_executor;
    co_await publishPromise_.getSemiFuture().via(exec);
    co_return co_await collectPhase();
  }

  folly::coro::Task<bool> collectPhase() {
    std::vector<ReceivedObject> received;
    bool drainedCatchup = false;
    while (!drainedCatchup) {
      auto obj = co_await lowConsumer_->dequeue();
      if (!obj) {
        break;
      }
      if (obj->group >= liveEdgeGroupID_) {
        obj->catchup = false;
        lowConsumer_->setCatchup(false);
        received.push_back(*obj);
        printEvent(folly::dynamic::object("event", "object")(
            "track", "low")("group", obj->group)("object", obj->objectID));
        drainedCatchup = true;
      } else {
        received.push_back(*obj);
        printEvent(folly::dynamic::object("event", "catchup")(
            "track", "low")("group", obj->group)("object", obj->objectID));
      }
    }

    int liveCollected = drainedCatchup ? 1 : 0;
    while (liveCollected < FLAGS_collect_groups) {
      auto obj = co_await lowConsumer_->dequeue();
      if (!obj) {
        break;
      }
      received.push_back(*obj);
      printEvent(folly::dynamic::object("event", "object")(
          "track", "low")("group", obj->group)("object", obj->objectID));
      ++liveCollected;
    }

    co_return emitResult(received);
  }

 private:
  static void printEvent(const folly::dynamic& d) {
    printf("%s\n", folly::toJson(d).c_str());
    fflush(stdout);
  }

  bool emitResult(const std::vector<ReceivedObject>& objs) {
    bool hasSwitchTransition = (switchingGroupID_ != UINT64_MAX);
    bool gap = false;
    bool duplicate = false;
    std::set<uint64_t> seen;
    uint64_t lastGroup = 0;
    bool first = true;
    for (const auto& o : objs) {
      if (seen.count(o.group)) {
        duplicate = true;
      }
      if (!first && o.group > lastGroup + 1) {
        gap = true;
      }
      if (!o.isLow && o.group >= switchingGroupID_) {
        gap = true;
      }
      if (o.isLow && o.group < switchingGroupID_) {
        duplicate = true;
      }
      seen.insert(o.group);
      lastGroup = o.group;
      first = false;
    }
    uint64_t catchupGroups = (liveEdgeGroupID_ > switchingGroupID_)
        ? liveEdgeGroupID_ - switchingGroupID_
        : 0;
    bool insufficientCatchup =
        (FLAGS_lag_seconds > 0 && catchupGroups == 0);
    bool pass = hasSwitchTransition && !gap && !duplicate && !insufficientCatchup;
    printEvent(folly::dynamic::object("event", "result")("pass", pass)(
        "g_switch", switchingGroupID_)("live_edge", liveEdgeGroupID_)(
        "catchup_groups", catchupGroups)("gap", gap)("duplicate", duplicate));
    return pass;
  }

  std::shared_ptr<MoQRelayClient> relayClient_;
  std::shared_ptr<CollectingConsumer> highConsumer_;
  std::shared_ptr<CollectingConsumer> lowConsumer_;
  // Held on the heap (not in coroutine frame) to keep it alive across co_awaits
  // and avoid GCC 12 ICE; used to unsubscribe the cache-priming subscription
  // for "low" before SWITCH is sent.
  std::shared_ptr<SubscriptionHandle> lowPrimeHandle_;
  RequestID highRequestID_{};
  uint64_t lastHighGroup_{0};
  folly::Promise<folly::Unit> publishPromise_;
  uint64_t switchingGroupID_{UINT64_MAX};
  uint64_t liveEdgeGroupID_{0};
};

} // namespace

int main(int argc, char* argv[]) {
  folly::Init init(&argc, &argv);
  auto sub = std::make_shared<SwitchSubscriber>();
  // ScopedEventBaseThread provides a running EventBase on a dedicated thread.
  // EventBaseManager::get()->getEventBase() in run() returns this EventBase,
  // making QUIC callbacks fire correctly (undriven EventBase = silent hang).
  folly::ScopedEventBaseThread ioThread;
  bool pass = folly::coro::blockingWait(
      sub->run().scheduleOn(ioThread.getEventBase()).start());
  return pass ? 0 : 1;
}
