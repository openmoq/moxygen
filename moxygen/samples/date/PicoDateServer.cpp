/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// Picoquic-based MoQ Date Server
//
// A simplified date server that uses PicoquicMoQServer for the QUIC transport.
// Publishes the current date/time every second on track "date" in a
// configurable namespace (default "moq-date").
//
// This is the std-mode (no Folly) equivalent of MoQDateServer.cpp.

#include <moxygen/MoQConsumers.h>
#include <moxygen/MoQSession.h>
#include <moxygen/MoQTypes.h>
#include <moxygen/MoQVersions.h>
#include <moxygen/Publisher.h>
#include <moxygen/compat/Callbacks.h>
#include <moxygen/compat/Debug.h>
#include <moxygen/compat/MoQServerInterface.h>
#include <moxygen/compat/SocketAddress.h>
#include <moxygen/events/MoQSimpleExecutor.h>
#include <moxygen/transports/PicoquicMoQServer.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <map>
#include <vector>

namespace {

using namespace moxygen;

// --- Command line arguments ---

struct Args {
  uint16_t port{4433};
  std::string certFile;
  std::string keyFile;
  std::string ns{"moq-date"};
  std::string mode{"spg"}; // spg, spo, datagram
};

Args parseArgs(int argc, char* argv[]) {
  Args args;
  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    if ((arg == "--port" || arg == "-p") && i + 1 < argc) {
      args.port = static_cast<uint16_t>(std::stoi(argv[++i]));
    } else if ((arg == "--cert" || arg == "-c") && i + 1 < argc) {
      args.certFile = argv[++i];
    } else if ((arg == "--key" || arg == "-k") && i + 1 < argc) {
      args.keyFile = argv[++i];
    } else if ((arg == "--ns" || arg == "-n") && i + 1 < argc) {
      args.ns = argv[++i];
    } else if ((arg == "--mode" || arg == "-m") && i + 1 < argc) {
      args.mode = argv[++i];
    } else if (arg == "--help" || arg == "-h") {
      std::cout << "Usage: picodateserver [options]\n"
                << "  --port, -p <port>    Server port (default: 4433)\n"
                << "  --cert, -c <file>    TLS certificate file (required)\n"
                << "  --key,  -k <file>    TLS private key file (required)\n"
                << "  --ns,   -n <ns>      Track namespace (default: moq-date)\n"
                << "  --mode, -m <mode>    spg|spo|datagram (default: spg)\n"
                << "  --help, -h           Show this help\n";
      std::exit(0);
    } else {
      std::cerr << "Unknown argument: " << arg << "\n";
      std::exit(1);
    }
  }
  if (args.certFile.empty() || args.keyFile.empty()) {
    std::cerr << "Error: --cert and --key are required\n";
    std::exit(1);
  }
  return args;
}

// --- Transmission Mode ---

enum class TransmitMode { STREAM_PER_GROUP, STREAM_PER_OBJECT, DATAGRAM };

TransmitMode parseMode(const std::string& mode) {
  if (mode == "spg") return TransmitMode::STREAM_PER_GROUP;
  if (mode == "spo") return TransmitMode::STREAM_PER_OBJECT;
  if (mode == "datagram") return TransmitMode::DATAGRAM;
  std::cerr << "Invalid mode: " << mode << " (use spg, spo, or datagram)\n";
  std::exit(1);
}

// --- DateSubscriptionHandle ---

class DateSubscriptionHandle : public Publisher::SubscriptionHandle {
 public:
  explicit DateSubscriptionHandle(SubscribeOk ok)
      : Publisher::SubscriptionHandle(std::move(ok)) {}

  void unsubscribe() override {
    unsubscribed_ = true;
  }

  void subscribeUpdateWithCallback(
      SubscribeUpdate update,
      std::shared_ptr<
          compat::ResultCallback<SubscribeUpdateOk, SubscribeUpdateError>>
          callback) override {
    callback->onError(SubscribeUpdateError{
        update.requestID,
        SubscribeUpdateErrorCode::NOT_SUPPORTED,
        "Subscribe update not supported"});
  }

  bool isUnsubscribed() const {
    return unsubscribed_;
  }

 private:
  std::atomic<bool> unsubscribed_{false};
};

// --- DatePublisher ---

class DatePublisher : public Publisher,
                      public std::enable_shared_from_this<DatePublisher> {
 public:
  DatePublisher(
      TransmitMode mode,
      std::string ns,
      std::shared_ptr<MoQSimpleExecutor> executor)
      : mode_(mode), ns_(std::move(ns)), executor_(std::move(executor)) {}

  FullTrackName dateTrackName() const {
    return FullTrackName({TrackNamespace({ns_}), "date"});
  }

  std::pair<uint64_t, uint64_t> now() const {
    auto nowTime = std::chrono::system_clock::now();
    auto inTimeT = std::chrono::system_clock::to_time_t(nowTime);
    return {static_cast<uint64_t>(inTimeT / 60),
            static_cast<uint64_t>(inTimeT % 60)};
  }

  AbsoluteLocation nowLocation() const {
    auto [minute, second] = now();
    return {minute, second + 1};
  }

  Payload minutePayload(uint64_t group) const {
    time_t inTimeT = static_cast<time_t>(group * 60);
    struct tm localTm;
    auto* lt = ::localtime_r(&inTimeT, &localTm);
    std::ostringstream ss;
    ss << std::put_time(lt, "%Y-%m-%d %H:%M:");
    return compat::Payload::copyBuffer(ss.str());
  }

  Payload secondPayload(uint64_t object) const {
    auto secStr = std::to_string(object - 1);
    return compat::Payload::copyBuffer(secStr);
  }

  // --- Publisher interface (callback-based, std-mode) ---

  void trackStatusWithCallback(
      const TrackStatus trackStatus,
      std::shared_ptr<compat::ResultCallback<TrackStatusOk, TrackStatusError>>
          callback) override {
    if (trackStatus.fullTrackName != dateTrackName()) {
      callback->onError(TrackStatusError{
          trackStatus.requestID,
          TrackStatusErrorCode::TRACK_NOT_EXIST,
          "The requested track does not exist"});
      return;
    }

    auto largest = nowLocation();
    callback->onSuccess(TrackStatusOk{
        .requestID = trackStatus.requestID,
        .expires = std::chrono::milliseconds(0),
        .groupOrder = GroupOrder::OldestFirst,
        .largest = largest,
        .fullTrackName = trackStatus.fullTrackName,
        .statusCode = TrackStatusCode::IN_PROGRESS});
  }

  void subscribeWithCallback(
      SubscribeRequest subReq,
      std::shared_ptr<TrackConsumer> consumer,
      std::shared_ptr<compat::ResultCallback<
          std::shared_ptr<SubscriptionHandle>,
          SubscribeError>> callback) override {
    XLOG(INFO) << "SubscribeRequest track ns="
               << subReq.fullTrackName.trackNamespace
               << " name=" << subReq.fullTrackName.trackName
               << " requestID=" << subReq.requestID << "\n";

    if (subReq.fullTrackName != dateTrackName()) {
      callback->onError(SubscribeError{
          subReq.requestID,
          SubscribeErrorCode::TRACK_NOT_EXIST,
          "unexpected subscribe"});
      return;
    }

    auto alias = TrackAlias(subReq.requestID.value);
    consumer->setTrackAlias(alias);

    auto largest = nowLocation();
    auto handle = std::make_shared<DateSubscriptionHandle>(SubscribeOk{
        subReq.requestID,
        alias,
        std::chrono::milliseconds(0),
        GroupOrder::OldestFirst,
        largest,
        Extensions{},
        TrackRequestParameters{FrameType::SUBSCRIBE_OK}});

    // Store subscriber
    {
      std::lock_guard<std::mutex> lock(subscribersMutex_);
      subscribers_[subReq.requestID] = SubscriberState{
          subReq, consumer, handle};
    }

    // Start the publish loop if not already running
    if (!publishLoopRunning_.exchange(true)) {
      startPublishLoop();
    }

    callback->onSuccess(handle);
  }

  void fetchWithCallback(
      Fetch fetch,
      std::shared_ptr<FetchConsumer> consumer,
      std::shared_ptr<
          compat::ResultCallback<std::shared_ptr<FetchHandle>, FetchError>>
          callback) override {
    XLOG(INFO) << "Fetch track ns=" << fetch.fullTrackName.trackNamespace
               << " name=" << fetch.fullTrackName.trackName
               << " requestID=" << fetch.requestID << "\n";

    if (fetch.fullTrackName != dateTrackName()) {
      callback->onError(FetchError{
          fetch.requestID,
          FetchErrorCode::TRACK_NOT_EXIST,
          "unexpected fetch"});
      return;
    }

    // Only support standalone fetches
    auto [standalone, joining] = fetchType(fetch);
    if (!standalone) {
      callback->onError(FetchError{
          fetch.requestID,
          FetchErrorCode::NOT_SUPPORTED,
          "only standalone fetches supported"});
      return;
    }

    auto largest = nowLocation();

    // Validate range
    if (standalone->start > largest) {
      callback->onError(FetchError{
          fetch.requestID,
          FetchErrorCode::INVALID_RANGE,
          "fetch starts in future"});
      return;
    }

    auto end = standalone->end;
    if (end > largest) {
      end = largest;
      end.object++;
    }

    if (end < standalone->start) {
      callback->onError(FetchError{
          fetch.requestID,
          FetchErrorCode::INVALID_RANGE,
          "No objects"});
      return;
    }

    // Send fetch data inline
    sendFetchData(consumer, standalone->start, end);

    class SimpleFetchHandle : public FetchHandle {
     public:
      explicit SimpleFetchHandle(FetchOk ok)
          : FetchHandle(std::move(ok)) {}
      void fetchCancel() override {}
    };

    auto fetchHandle = std::make_shared<SimpleFetchHandle>(FetchOk{
        fetch.requestID,
        resolveGroupOrder(GroupOrder::OldestFirst, fetch.groupOrder),
        0,
        largest,
        Extensions{},
        TrackRequestParameters{FrameType::FETCH_OK}});
    callback->onSuccess(fetchHandle);
  }

 private:
  static GroupOrder resolveGroupOrder(
      GroupOrder pubOrder,
      GroupOrder subOrder) {
    if (subOrder != GroupOrder::Default) {
      return subOrder;
    }
    return pubOrder;
  }

  struct SubscriberState {
    SubscribeRequest request;
    std::shared_ptr<TrackConsumer> consumer;
    std::shared_ptr<DateSubscriptionHandle> handle;
    std::shared_ptr<SubgroupConsumer> subgroupPublisher;
    uint64_t currentMinute{0};
  };

  void startPublishLoop() {
    // Schedule the first tick
    auto weakSelf = weak_from_this();
    executor_->scheduleTimeout(
        [weakSelf]() {
          if (auto self = weakSelf.lock()) {
            self->publishTick();
          }
        },
        std::chrono::milliseconds(1000));
  }

  void publishTick() {
    auto [minute, second] = now();

    {
      std::lock_guard<std::mutex> lock(subscribersMutex_);

      // Remove unsubscribed entries
      for (auto it = subscribers_.begin(); it != subscribers_.end();) {
        if (it->second.handle->isUnsubscribed()) {
          it = subscribers_.erase(it);
        } else {
          ++it;
        }
      }

      // Publish to all active subscribers
      for (auto& [reqId, sub] : subscribers_) {
        publishToSubscriber(sub, minute, second);
      }
    }

    // Schedule next tick
    auto weakSelf = weak_from_this();
    executor_->scheduleTimeout(
        [weakSelf]() {
          if (auto self = weakSelf.lock()) {
            self->publishTick();
          }
        },
        std::chrono::milliseconds(1000));
  }

  void publishToSubscriber(
      SubscriberState& sub,
      uint64_t minute,
      uint64_t second) {
    switch (mode_) {
      case TransmitMode::STREAM_PER_GROUP:
        publishStreamPerGroup(sub, minute, second);
        break;
      case TransmitMode::STREAM_PER_OBJECT:
        publishStreamPerObject(sub, minute, second);
        break;
      case TransmitMode::DATAGRAM:
        publishDatagram(sub, minute, second);
        break;
    }
  }

  void publishStreamPerGroup(
      SubscriberState& sub,
      uint64_t group,
      uint64_t second) {
    uint64_t subgroupId = 0;
    uint64_t objectId = second;

    // Start new subgroup on minute boundary or if none exists
    if (!sub.subgroupPublisher || group != sub.currentMinute) {
      auto result =
          sub.consumer->beginSubgroup(group, subgroupId, /*priority=*/128);
      if (result.hasError()) {
        XLOG(ERR) << "beginSubgroup error: " << result.error().what() << "\n";
        return;
      }
      sub.subgroupPublisher = result.value();
      sub.currentMinute = group;
    }

    if (objectId == 0) {
      auto res =
          sub.subgroupPublisher->object(0, minutePayload(group), noExtensions());
      if (res.hasError()) {
        XLOG(ERR) << "object(minute) error: " << res.error().what() << "\n";
      }
    }

    objectId++;
    auto res = sub.subgroupPublisher->object(
        objectId, secondPayload(objectId), noExtensions());
    if (res.hasError()) {
      XLOG(ERR) << "object(second) error: " << res.error().what() << "\n";
    }

    if (objectId >= 60) {
      objectId++;
      sub.subgroupPublisher->endOfGroup(objectId);
      sub.subgroupPublisher.reset();
    }
  }

  void publishStreamPerObject(
      SubscriberState& sub,
      uint64_t group,
      uint64_t second) {
    uint64_t subgroupId = second;
    uint64_t objectId = second;

    ObjectHeader header{
        group,
        subgroupId,
        objectId,
        /*priorityIn=*/128,
        ObjectStatus::NORMAL,
        noExtensions(),
        std::nullopt};

    if (second == 0) {
      sub.consumer->objectStream(header, minutePayload(group));
    }

    header.subgroup++;
    header.id++;
    sub.consumer->objectStream(header, secondPayload(header.id));

    if (header.id >= 60) {
      header.subgroup++;
      header.id++;
      header.status = ObjectStatus::END_OF_GROUP;
      sub.consumer->objectStream(header, nullptr);
    }
  }

  void publishDatagram(
      SubscriberState& sub,
      uint64_t group,
      uint64_t second) {
    uint64_t objectId = second;

    ObjectHeader header{
        group,
        0,
        objectId,
        /*priorityIn=*/128,
        ObjectStatus::NORMAL,
        noExtensions(),
        std::nullopt};

    if (second == 0) {
      sub.consumer->datagram(header, minutePayload(group));
    }

    header.id++;
    sub.consumer->datagram(header, secondPayload(header.id));

    if (header.id >= 60) {
      header.id++;
      header.status = ObjectStatus::END_OF_GROUP;
      sub.consumer->datagram(header, nullptr);
    }
  }

  void sendFetchData(
      std::shared_ptr<FetchConsumer> consumer,
      AbsoluteLocation start,
      AbsoluteLocation end) {
    auto cur = start;
    while (cur < end) {
      uint64_t subgroup =
          mode_ == TransmitMode::STREAM_PER_OBJECT ? cur.object : 0;

      if (cur.object == 0) {
        auto res = consumer->object(
            cur.group, subgroup, cur.object, minutePayload(cur.group));
        if (res.hasError()) {
          XLOG(ERR) << "fetch object error: " << res.error().what() << "\n";
          consumer->reset(ResetStreamErrorCode::INTERNAL_ERROR);
          return;
        }
      } else if (cur.object <= 60) {
        auto res = consumer->object(
            cur.group, subgroup, cur.object, secondPayload(cur.object));
        if (res.hasError()) {
          XLOG(ERR) << "fetch object error: " << res.error().what() << "\n";
          consumer->reset(ResetStreamErrorCode::INTERNAL_ERROR);
          return;
        }
      } else {
        auto res = consumer->endOfGroup(cur.group, subgroup, cur.object);
        if (res.hasError()) {
          XLOG(ERR) << "fetch endOfGroup error: " << res.error().what()
                    << "\n";
          consumer->reset(ResetStreamErrorCode::INTERNAL_ERROR);
          return;
        }
      }

      cur.object++;
      if (cur.object > 61) {
        cur.group++;
        cur.object = 0;
      }
    }

    consumer->endOfFetch();
  }

  TransmitMode mode_;
  std::string ns_;
  std::shared_ptr<MoQSimpleExecutor> executor_;

  std::mutex subscribersMutex_;
  std::map<RequestID, SubscriberState> subscribers_;
  std::atomic<bool> publishLoopRunning_{false};
};

// --- Session Handler ---

class DateServerSessionHandler
    : public compat::MoQServerInterface::SessionHandler {
 public:
  explicit DateServerSessionHandler(
      std::shared_ptr<DatePublisher> publisher)
      : publisher_(std::move(publisher)) {}

  compat::Expected<ServerSetup, SessionCloseErrorCode> onNewSession(
      std::shared_ptr<MoQSession> session,
      const ClientSetup& clientSetup) override {
    XLOG(INFO) << "New client session\n";

    // Set publisher as the publish handler for this session
    session->setPublishHandler(publisher_);

    // Negotiate version
    uint64_t selectedVersion = kVersionDraftCurrent;
    for (auto v : clientSetup.supportedVersions) {
      if (isSupportedVersion(v)) {
        selectedVersion = v;
        break;
      }
    }

    ServerSetup setup;
    setup.selectedVersion = selectedVersion;
    return setup;
  }

  void onSessionTerminated(std::shared_ptr<MoQSession> session) override {
    XLOG(INFO) << "Client session terminated\n";
  }

 private:
  std::shared_ptr<DatePublisher> publisher_;
};

// --- Signal handling ---

std::atomic<bool> g_running{true};

void signalHandler(int /*sig*/) {
  g_running.store(false);
}

} // namespace

int main(int argc, char* argv[]) {
  auto args = parseArgs(argc, argv);
  auto mode = parseMode(args.mode);

  std::cout << "Starting picodateserver on port " << args.port
            << " with namespace '" << args.ns << "' mode=" << args.mode
            << "\n";

  // Create executor
  auto executor = std::make_shared<moxygen::MoQSimpleExecutor>();

  // Create publisher
  auto publisher = std::make_shared<DatePublisher>(mode, args.ns, executor);

  // Create server config
  moxygen::transports::PicoquicMoQServer::Config serverConfig;
  serverConfig.certFile = args.certFile;
  serverConfig.keyFile = args.keyFile;
  serverConfig.alpn = std::string(moxygen::kAlpnMoqtLegacy);

  // Create server
  auto server = std::make_unique<moxygen::transports::PicoquicMoQServer>(
      executor, std::move(serverConfig));

  // Create session handler
  auto handler = std::make_shared<DateServerSessionHandler>(publisher);

  // Set up signal handling
  std::signal(SIGINT, signalHandler);
  std::signal(SIGTERM, signalHandler);

  // Start server
  moxygen::compat::SocketAddress addr("::", args.port);
  server->start(addr, handler);

  std::cout << "Server listening on port " << args.port << "\n";

  // Run executor event loop (processes scheduled tasks like the publish loop)
  // The server runs its own thread for the QUIC event loop, so we use the
  // executor loop here for application-level scheduling.
  while (g_running.load()) {
    // Process pending executor tasks with a short timeout
    executor->scheduleTimeout(
        [&]() {
          if (!g_running.load()) {
            executor->stop();
          }
        },
        std::chrono::milliseconds(500));

    // Run a batch of tasks (this will block until tasks are available or
    // the stop signal)
    executor->run();
  }

  std::cout << "Shutting down...\n";
  server->stop();

  return 0;
}
