/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

// Integration test base for picoquic-based MoQ tests
// These tests use real network connections to verify end-to-end functionality.

#include <moxygen/compat/Config.h>

#if MOXYGEN_QUIC_PICOQUIC

#include <moxygen/MoQConsumers.h>
#include <moxygen/MoQSession.h>
#include <moxygen/MoQTypes.h>
#include <moxygen/MoQVersions.h>
#include <moxygen/Publisher.h>
#include <moxygen/Subscriber.h>
#include <moxygen/compat/Callbacks.h>
#include <moxygen/compat/MoQClientInterface.h>
#include <moxygen/compat/MoQServerInterface.h>
#include <moxygen/compat/SocketAddress.h>
#include <moxygen/events/MoQSimpleExecutor.h>
#include <moxygen/transports/PicoquicMoQClient.h>
#include <moxygen/transports/PicoquicMoQServer.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace moxygen::test::integration {

// Default timeouts for integration tests
constexpr auto kConnectTimeout = std::chrono::milliseconds(5000);
constexpr auto kTestTimeout = std::chrono::seconds(10);

/**
 * Simple test publisher that sends numbered objects on demand.
 */
class TestPublisher : public Publisher,
                      public std::enable_shared_from_this<TestPublisher> {
 public:
  TestPublisher(
      std::shared_ptr<MoQSimpleExecutor> executor,
      TrackNamespace trackNamespace,
      std::string trackName);

  FullTrackName getFullTrackName() const {
    return FullTrackName({trackNamespace_, trackName_});
  }

  // Publisher interface
  void trackStatusWithCallback(
      const TrackStatus trackStatus,
      std::shared_ptr<compat::ResultCallback<TrackStatusOk, TrackStatusError>>
          callback) override;

  void subscribeWithCallback(
      SubscribeRequest subReq,
      std::shared_ptr<TrackConsumer> consumer,
      std::shared_ptr<compat::ResultCallback<
          std::shared_ptr<SubscriptionHandle>,
          SubscribeError>> callback) override;

  void fetchWithCallback(
      Fetch fetch,
      std::shared_ptr<FetchConsumer> consumer,
      std::shared_ptr<
          compat::ResultCallback<std::shared_ptr<FetchHandle>, FetchError>>
          callback) override;

  // Send a test object to all subscribers
  void publishObject(uint64_t group, uint64_t object, const std::string& data);

  // Get count of active subscriptions
  size_t getSubscriptionCount() const;

 private:
  class TestSubscriptionHandle : public SubscriptionHandle {
   public:
    explicit TestSubscriptionHandle(SubscribeOk ok)
        : SubscriptionHandle(std::move(ok)) {}

    void unsubscribe() override { unsubscribed_ = true; }

    void subscribeUpdateWithCallback(
        SubscribeUpdate update,
        std::shared_ptr<
            compat::ResultCallback<SubscribeUpdateOk, SubscribeUpdateError>>
            callback) override {
      callback->onError(SubscribeUpdateError{
          update.requestID,
          SubscribeUpdateErrorCode::NOT_SUPPORTED,
          "Not supported"});
    }

    bool isUnsubscribed() const { return unsubscribed_; }

   private:
    std::atomic<bool> unsubscribed_{false};
  };

  struct SubscriberState {
    std::shared_ptr<TrackConsumer> consumer;
    std::shared_ptr<TestSubscriptionHandle> handle;
    std::shared_ptr<SubgroupConsumer> subgroup;
    uint64_t currentGroup{0};
  };

  std::shared_ptr<MoQSimpleExecutor> executor_;
  TrackNamespace trackNamespace_;
  std::string trackName_;

  mutable std::mutex mutex_;
  std::vector<SubscriberState> subscribers_;
  uint64_t nextRequestId_{1};
};

/**
 * Simple test subscriber that accepts any namespace announcement.
 */
class TestSubscriber : public Subscriber {
 public:
  void publishNamespaceWithCallback(
      PublishNamespace ann,
      std::shared_ptr<PublishNamespaceCallback> cancelCallback,
      std::shared_ptr<compat::ResultCallback<
          std::shared_ptr<PublishNamespaceHandle>,
          PublishNamespaceError>> callback) override;

  PublishResult publish(
      PublishRequest pub,
      std::shared_ptr<SubscriptionHandle> handle) override;
};

/**
 * Simple track consumer that collects received objects.
 */
class TestTrackConsumer : public TrackConsumer {
 public:
  struct ReceivedObject {
    uint64_t group;
    uint64_t subgroup;
    uint64_t object;
    std::string data;
  };

  compat::Expected<compat::Unit, MoQPublishError> setTrackAlias(
      TrackAlias alias) override;

  compat::Expected<std::shared_ptr<SubgroupConsumer>, MoQPublishError>
  beginSubgroup(
      uint64_t groupID,
      uint64_t subgroupID,
      Priority priority,
      bool containsLastInGroup = false) override;

  compat::Expected<compat::SemiFuture<compat::Unit>, MoQPublishError>
  awaitStreamCredit() override;

  compat::Expected<compat::Unit, MoQPublishError> objectStream(
      const ObjectHeader& header,
      Payload payload,
      bool lastInGroup = false) override;

  compat::Expected<compat::Unit, MoQPublishError> datagram(
      const ObjectHeader& header,
      Payload payload,
      bool lastInGroup = false) override;

  compat::Expected<compat::Unit, MoQPublishError> publishDone(
      PublishDone pubDone) override;

  // Access received objects
  std::vector<ReceivedObject> getObjects() const;
  size_t getObjectCount() const;
  bool waitForObjects(
      size_t count,
      std::chrono::milliseconds timeout = std::chrono::seconds(5));

  bool isPublishDone() const { return publishDone_; }

 private:
  class TestSubgroupConsumer : public SubgroupConsumer {
   public:
    TestSubgroupConsumer(TestTrackConsumer* parent, uint64_t group, uint64_t subgroup)
        : parent_(parent), group_(group), subgroup_(subgroup) {}

    compat::Expected<compat::Unit, MoQPublishError> object(
        uint64_t objectID,
        Payload payload,
        Extensions extensions,
        bool finSubgroup) override;

    compat::Expected<compat::Unit, MoQPublishError> beginObject(
        uint64_t objectID,
        uint64_t length,
        Payload initialPayload,
        Extensions extensions) override;

    compat::Expected<ObjectPublishStatus, MoQPublishError> objectPayload(
        Payload payload,
        bool finSubgroup) override;

    compat::Expected<compat::Unit, MoQPublishError> endOfGroup(
        uint64_t endOfGroupObjectID) override;

    compat::Expected<compat::Unit, MoQPublishError> endOfTrackAndGroup(
        uint64_t endOfTrackObjectID) override;

    compat::Expected<compat::Unit, MoQPublishError> endOfSubgroup() override;

    void reset(ResetStreamErrorCode error) override;

   private:
    TestTrackConsumer* parent_;
    uint64_t group_;
    uint64_t subgroup_;
  };

  void addObject(uint64_t group, uint64_t subgroup, uint64_t object, std::string data);

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::vector<ReceivedObject> objects_;
  std::optional<TrackAlias> trackAlias_;
  std::atomic<bool> publishDone_{false};
};

/**
 * Session handler for test server.
 */
class TestSessionHandler : public compat::MoQServerInterface::SessionHandler {
 public:
  explicit TestSessionHandler(std::shared_ptr<TestPublisher> publisher)
      : publisher_(std::move(publisher)) {}

  compat::Expected<ServerSetup, SessionCloseErrorCode> onNewSession(
      std::shared_ptr<MoQSession> session,
      const ClientSetup& clientSetup) override;

  void onSessionTerminated(std::shared_ptr<MoQSession> session) override;

  size_t getActiveSessionCount() const;

 private:
  std::shared_ptr<TestPublisher> publisher_;
  mutable std::mutex mutex_;
  std::vector<std::shared_ptr<MoQSession>> sessions_;
};

/**
 * Base class for picoquic integration tests.
 *
 * Provides:
 * - Server startup with test publisher
 * - Client connection helpers
 * - Test certificate paths
 * - Utilities for waiting on async conditions
 */
class IntegrationTestBase : public ::testing::Test {
 public:
  void SetUp() override;
  void TearDown() override;

 protected:
  // Get test certificate paths (from picoquic test certs)
  const std::string& getCertPath() const { return certPath_; }
  const std::string& getKeyPath() const { return keyPath_; }

  // Start a test server
  void startServer(
      uint16_t port,
      const TrackNamespace& trackNamespace,
      const std::string& trackName);

  // Stop the server
  void stopServer();

  // Get the test publisher (for sending objects)
  std::shared_ptr<TestPublisher> getPublisher() { return publisher_; }

  // Create a client connected to the test server
  std::unique_ptr<transports::PicoquicMoQClient> createClient(
      const std::string& host,
      uint16_t port);

  // Connect client with callback
  bool connectClient(
      transports::PicoquicMoQClient& client,
      std::shared_ptr<TestSubscriber> subscriber,
      std::shared_ptr<MoQSession>& outSession);

  // Subscribe to a track
  bool subscribe(
      MoQSession& session,
      const FullTrackName& trackName,
      std::shared_ptr<TestTrackConsumer> consumer,
      std::shared_ptr<Publisher::SubscriptionHandle>& outHandle);

  // Wait for a condition with timeout
  template <typename Pred>
  bool waitFor(Pred pred, std::chrono::milliseconds timeout = kTestTimeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      if (pred()) {
        return true;
      }
      // Run executor briefly
      executor_->runFor(std::chrono::milliseconds(10));
    }
    return pred();
  }

  // Get the executor
  std::shared_ptr<MoQSimpleExecutor> getExecutor() { return executor_; }

  // Find an available port (simple implementation)
  static uint16_t findAvailablePort();

 private:
  std::shared_ptr<MoQSimpleExecutor> executor_;
  std::unique_ptr<transports::PicoquicMoQServer> server_;
  std::shared_ptr<TestPublisher> publisher_;
  std::shared_ptr<TestSessionHandler> sessionHandler_;

  std::string certPath_;
  std::string keyPath_;
};

} // namespace moxygen::test::integration

#endif // MOXYGEN_QUIC_PICOQUIC
