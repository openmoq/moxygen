/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "moxygen/test/integration/IntegrationTestBase.h"

#if MOXYGEN_QUIC_PICOQUIC

#include <cstdlib>
#include <fstream>
#include <random>

namespace moxygen::test::integration {

// ============================================================================
// TestPublisher implementation
// ============================================================================

TestPublisher::TestPublisher(
    std::shared_ptr<MoQSimpleExecutor> executor,
    TrackNamespace trackNamespace,
    std::string trackName)
    : executor_(std::move(executor)),
      trackNamespace_(std::move(trackNamespace)),
      trackName_(std::move(trackName)) {}

void TestPublisher::trackStatusWithCallback(
    const TrackStatus trackStatus,
    std::shared_ptr<compat::ResultCallback<TrackStatusOk, TrackStatusError>>
        callback) {
  if (trackStatus.fullTrackName != getFullTrackName()) {
    callback->onError(TrackStatusError{
        trackStatus.requestID,
        TrackStatusErrorCode::TRACK_NOT_EXIST,
        "Track not found"});
    return;
  }

  callback->onSuccess(TrackStatusOk{
      .requestID = trackStatus.requestID,
      .expires = std::chrono::milliseconds(0),
      .groupOrder = GroupOrder::OldestFirst,
      .largest = AbsoluteLocation{0, 0},
      .fullTrackName = trackStatus.fullTrackName,
      .statusCode = TrackStatusCode::IN_PROGRESS});
}

void TestPublisher::subscribeWithCallback(
    SubscribeRequest subReq,
    std::shared_ptr<TrackConsumer> consumer,
    std::shared_ptr<
        compat::ResultCallback<std::shared_ptr<SubscriptionHandle>, SubscribeError>>
        callback) {
  if (subReq.fullTrackName != getFullTrackName()) {
    callback->onError(SubscribeError{
        subReq.requestID,
        SubscribeErrorCode::TRACK_NOT_EXIST,
        "Track not found"});
    return;
  }

  auto alias = TrackAlias(subReq.requestID.value);
  consumer->setTrackAlias(alias);

  auto handle = std::make_shared<TestSubscriptionHandle>(SubscribeOk{
      subReq.requestID,
      alias,
      std::chrono::milliseconds(0),
      GroupOrder::OldestFirst,
      std::nullopt,
      Extensions{},
      TrackRequestParameters{FrameType::SUBSCRIBE_OK}});

  {
    std::lock_guard<std::mutex> lock(mutex_);
    subscribers_.push_back(SubscriberState{consumer, handle, nullptr, 0});
  }

  callback->onSuccess(handle);
}

void TestPublisher::fetchWithCallback(
    Fetch fetch,
    std::shared_ptr<FetchConsumer> consumer,
    std::shared_ptr<
        compat::ResultCallback<std::shared_ptr<FetchHandle>, FetchError>>
        callback) {
  callback->onError(FetchError{
      fetch.requestID,
      FetchErrorCode::NOT_SUPPORTED,
      "Fetch not supported in test publisher"});
}

void TestPublisher::publishObject(
    uint64_t group,
    uint64_t object,
    const std::string& data) {
  std::lock_guard<std::mutex> lock(mutex_);

  // Remove unsubscribed entries
  subscribers_.erase(
      std::remove_if(
          subscribers_.begin(),
          subscribers_.end(),
          [](const SubscriberState& s) { return s.handle->isUnsubscribed(); }),
      subscribers_.end());

  for (auto& sub : subscribers_) {
    // Create new subgroup if needed
    if (!sub.subgroup || sub.currentGroup != group) {
      auto result = sub.consumer->beginSubgroup(group, 0, 128);
      if (result.hasError()) {
        continue;
      }
      sub.subgroup = result.value();
      sub.currentGroup = group;
    }

    auto payload = compat::Payload::copyBuffer(data);
    sub.subgroup->object(object, std::move(payload), noExtensions(), false);
  }
}

size_t TestPublisher::getSubscriptionCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  size_t count = 0;
  for (const auto& sub : subscribers_) {
    if (!sub.handle->isUnsubscribed()) {
      count++;
    }
  }
  return count;
}

// ============================================================================
// TestSubscriber implementation
// ============================================================================

void TestSubscriber::publishNamespaceWithCallback(
    PublishNamespace ann,
    std::shared_ptr<PublishNamespaceCallback> /*cancelCallback*/,
    std::shared_ptr<compat::ResultCallback<
        std::shared_ptr<PublishNamespaceHandle>,
        PublishNamespaceError>> callback) {
  callback->onSuccess(std::make_shared<PublishNamespaceHandle>(
      PublishNamespaceOk{
          .requestID = ann.requestID,
          .requestSpecificParams = {}}));
}

Subscriber::PublishResult TestSubscriber::publish(
    PublishRequest pub,
    std::shared_ptr<SubscriptionHandle> /*handle*/) {
  return compat::makeUnexpected(PublishError{
      pub.requestID,
      PublishErrorCode::NOT_SUPPORTED,
      "Publish not supported"});
}

// ============================================================================
// TestTrackConsumer implementation
// ============================================================================

compat::Expected<compat::Unit, MoQPublishError>
TestTrackConsumer::setTrackAlias(TrackAlias alias) {
  trackAlias_ = alias;
  return compat::unit;
}

compat::Expected<std::shared_ptr<SubgroupConsumer>, MoQPublishError>
TestTrackConsumer::beginSubgroup(
    uint64_t groupID,
    uint64_t subgroupID,
    Priority /*priority*/,
    bool /*containsLastInGroup*/) {
  return std::make_shared<TestSubgroupConsumer>(this, groupID, subgroupID);
}

compat::Expected<compat::SemiFuture<compat::Unit>, MoQPublishError>
TestTrackConsumer::awaitStreamCredit() {
  return compat::makeSemiFuture();
}

compat::Expected<compat::Unit, MoQPublishError>
TestTrackConsumer::objectStream(
    const ObjectHeader& header,
    Payload payload,
    bool /*lastInGroup*/) {
  if (payload) {
    addObject(header.group, header.subgroup, header.id, payload->moveToString());
  }
  return compat::unit;
}

compat::Expected<compat::Unit, MoQPublishError>
TestTrackConsumer::datagram(
    const ObjectHeader& header,
    Payload payload,
    bool /*lastInGroup*/) {
  if (payload) {
    addObject(header.group, header.subgroup, header.id, payload->moveToString());
  }
  return compat::unit;
}

compat::Expected<compat::Unit, MoQPublishError>
TestTrackConsumer::publishDone(PublishDone /*pubDone*/) {
  publishDone_ = true;
  cv_.notify_all();
  return compat::unit;
}

std::vector<TestTrackConsumer::ReceivedObject>
TestTrackConsumer::getObjects() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return objects_;
}

size_t TestTrackConsumer::getObjectCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return objects_.size();
}

bool TestTrackConsumer::waitForObjects(
    size_t count,
    std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(mutex_);
  return cv_.wait_for(lock, timeout, [this, count]() {
    return objects_.size() >= count;
  });
}

void TestTrackConsumer::addObject(
    uint64_t group,
    uint64_t subgroup,
    uint64_t object,
    std::string data) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    objects_.push_back(ReceivedObject{group, subgroup, object, std::move(data)});
  }
  cv_.notify_all();
}

// TestSubgroupConsumer implementation

compat::Expected<compat::Unit, MoQPublishError>
TestTrackConsumer::TestSubgroupConsumer::object(
    uint64_t objectID,
    Payload payload,
    Extensions /*extensions*/,
    bool /*finSubgroup*/) {
  if (payload) {
    parent_->addObject(group_, subgroup_, objectID, payload->moveToString());
  }
  return compat::unit;
}

compat::Expected<compat::Unit, MoQPublishError>
TestTrackConsumer::TestSubgroupConsumer::beginObject(
    uint64_t /*objectID*/,
    uint64_t /*length*/,
    Payload /*initialPayload*/,
    Extensions /*extensions*/) {
  return compat::unit;
}

compat::Expected<ObjectPublishStatus, MoQPublishError>
TestTrackConsumer::TestSubgroupConsumer::objectPayload(
    Payload /*payload*/,
    bool /*finSubgroup*/) {
  return ObjectPublishStatus::DONE;
}

compat::Expected<compat::Unit, MoQPublishError>
TestTrackConsumer::TestSubgroupConsumer::endOfGroup(uint64_t /*endOfGroupObjectID*/) {
  return compat::unit;
}

compat::Expected<compat::Unit, MoQPublishError>
TestTrackConsumer::TestSubgroupConsumer::endOfTrackAndGroup(
    uint64_t /*endOfTrackObjectID*/) {
  return compat::unit;
}

compat::Expected<compat::Unit, MoQPublishError>
TestTrackConsumer::TestSubgroupConsumer::endOfSubgroup() {
  return compat::unit;
}

void TestTrackConsumer::TestSubgroupConsumer::reset(
    ResetStreamErrorCode /*error*/) {}

// ============================================================================
// TestSessionHandler implementation
// ============================================================================

compat::Expected<ServerSetup, SessionCloseErrorCode>
TestSessionHandler::onNewSession(
    std::shared_ptr<MoQSession> session,
    const ClientSetup& clientSetup) {
  session->setPublishHandler(publisher_);

  {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.push_back(session);
  }

  // Select version
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

void TestSessionHandler::onSessionTerminated(
    std::shared_ptr<MoQSession> session) {
  std::lock_guard<std::mutex> lock(mutex_);
  sessions_.erase(
      std::remove(sessions_.begin(), sessions_.end(), session),
      sessions_.end());
}

size_t TestSessionHandler::getActiveSessionCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return sessions_.size();
}

// ============================================================================
// IntegrationTestBase implementation
// ============================================================================

void IntegrationTestBase::SetUp() {
  // Create executor
  executor_ = std::make_shared<MoQSimpleExecutor>();

  // Find picoquic test certs
  // Check common locations for picoquic certs
  std::vector<std::string> certSearchPaths = {
#ifdef PICOQUIC_CERTS_DIR
      // Path from CMake compile definition
      PICOQUIC_CERTS_DIR,
#endif
      // Build directory (FetchContent)
      "_build_std/_deps/picoquic-src/certs",
      "../_build_std/_deps/picoquic-src/certs",
      "../../_build_std/_deps/picoquic-src/certs",
      "../../../_build_std/_deps/picoquic-src/certs",
      // Environment variable
      std::string(std::getenv("PICOQUIC_CERTS_DIR") ? std::getenv("PICOQUIC_CERTS_DIR") : ""),
      // Fallback to current directory
      "certs",
  };

  for (const auto& basePath : certSearchPaths) {
    if (basePath.empty()) continue;

    std::string testCert = basePath + "/cert.pem";
    std::string testKey = basePath + "/key.pem";

    std::ifstream certFile(testCert);
    std::ifstream keyFile(testKey);
    if (certFile.good() && keyFile.good()) {
      certPath_ = testCert;
      keyPath_ = testKey;
      break;
    }
  }

  // If not found, try the secp256r1 certs
  if (certPath_.empty()) {
    for (const auto& basePath : certSearchPaths) {
      if (basePath.empty()) continue;

      std::string testCert = basePath + "/secp256r1/cert.pem";
      std::string testKey = basePath + "/secp256r1/key.pem";

      std::ifstream certFile(testCert);
      std::ifstream keyFile(testKey);
      if (certFile.good() && keyFile.good()) {
        certPath_ = testCert;
        keyPath_ = testKey;
        break;
      }
    }
  }
}

void IntegrationTestBase::TearDown() {
  stopServer();
  executor_->stop();
}

void IntegrationTestBase::startServer(
    uint16_t port,
    const TrackNamespace& trackNamespace,
    const std::string& trackName) {
  ASSERT_FALSE(certPath_.empty()) << "Test certificates not found";
  ASSERT_FALSE(keyPath_.empty()) << "Test key not found";

  // Create publisher
  publisher_ = std::make_shared<TestPublisher>(executor_, trackNamespace, trackName);

  // Create session handler
  sessionHandler_ = std::make_shared<TestSessionHandler>(publisher_);

  // Create server config
  transports::PicoquicMoQServer::Config serverConfig;
  serverConfig.certFile = certPath_;
  serverConfig.keyFile = keyPath_;
  serverConfig.alpn = std::string(kAlpnMoqtLegacy);
  serverConfig.idleTimeout = std::chrono::milliseconds(30000);

  // Create and start server
  server_ = std::make_unique<transports::PicoquicMoQServer>(
      executor_, std::move(serverConfig));

  compat::SocketAddress addr("::", port);
  server_->start(addr, sessionHandler_);
}

void IntegrationTestBase::stopServer() {
  if (server_) {
    server_->stop();
    server_.reset();
  }
  sessionHandler_.reset();
  publisher_.reset();
}

std::unique_ptr<transports::PicoquicMoQClient>
IntegrationTestBase::createClient(const std::string& host, uint16_t port) {
  transports::PicoquicMoQClient::Config clientConfig;
  clientConfig.serverHost = host;
  clientConfig.serverPort = port;
  clientConfig.alpn = std::string(kAlpnMoqtLegacy);
  clientConfig.insecure = true;  // Skip cert validation for tests
  clientConfig.idleTimeout = std::chrono::milliseconds(30000);

  return std::make_unique<transports::PicoquicMoQClient>(
      executor_, std::move(clientConfig));
}

bool IntegrationTestBase::connectClient(
    transports::PicoquicMoQClient& client,
    std::shared_ptr<TestSubscriber> subscriber,
    std::shared_ptr<MoQSession>& outSession) {
  std::atomic<bool> connected{false};
  std::atomic<bool> failed{false};

  auto callback = std::make_shared<
      compat::LambdaResultCallback<
          compat::MoQClientInterface::ConnectResult,
          SessionCloseErrorCode>>(
      [&](compat::MoQClientInterface::ConnectResult result) {
        outSession = result.session;
        connected = true;
      },
      [&](SessionCloseErrorCode /*error*/) {
        failed = true;
      });

  client.connectWithCallback(kConnectTimeout, nullptr, subscriber, callback);

  // Wait for connection
  return waitFor([&]() { return connected.load() || failed.load(); }) &&
         connected.load();
}

bool IntegrationTestBase::subscribe(
    MoQSession& session,
    const FullTrackName& trackName,
    std::shared_ptr<TestTrackConsumer> consumer,
    std::shared_ptr<Publisher::SubscriptionHandle>& outHandle) {
  std::atomic<bool> subscribed{false};
  std::atomic<bool> failed{false};

  auto subReq = SubscribeRequest::make(
      trackName,
      kDefaultPriority,
      GroupOrder::OldestFirst,
      true,
      LocationType::LargestObject);

  auto callback = std::make_shared<
      compat::LambdaResultCallback<
          std::shared_ptr<Publisher::SubscriptionHandle>,
          SubscribeError>>(
      [&](std::shared_ptr<Publisher::SubscriptionHandle> handle) {
        outHandle = handle;
        subscribed = true;
      },
      [&](SubscribeError /*error*/) {
        failed = true;
      });

  session.subscribeWithCallback(std::move(subReq), consumer, callback);

  // Wait for subscription
  return waitFor([&]() { return subscribed.load() || failed.load(); }) &&
         subscribed.load();
}

uint16_t IntegrationTestBase::findAvailablePort() {
  // Use a random port in the ephemeral range
  static std::random_device rd;
  static std::mt19937 gen(rd());
  std::uniform_int_distribution<uint16_t> dis(49152, 65535);
  return dis(gen);
}

} // namespace moxygen::test::integration

#endif // MOXYGEN_QUIC_PICOQUIC
