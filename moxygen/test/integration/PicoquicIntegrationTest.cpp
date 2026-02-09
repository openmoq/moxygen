/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// Integration tests for picoquic-based MoQ transport.
// These tests verify end-to-end functionality with real network connections.

#include "moxygen/test/integration/IntegrationTestBase.h"

#if MOXYGEN_QUIC_PICOQUIC

#include <gtest/gtest.h>
#include <thread>

namespace moxygen::test::integration {

class PicoquicIntegrationTest : public IntegrationTestBase {
 protected:
  static constexpr uint16_t kBasePort = 14433;

  void SetUp() override {
    IntegrationTestBase::SetUp();
    // Use a different port for each test to avoid conflicts
    testPort_ = findAvailablePort();
  }

  uint16_t testPort_;
};

// Skip tests if certificates are not available
#define SKIP_IF_NO_CERTS()                          \
  do {                                              \
    if (getCertPath().empty()) {                    \
      GTEST_SKIP() << "Test certificates not found"; \
    }                                               \
  } while (0)

TEST_F(PicoquicIntegrationTest, ServerStartStop) {
  SKIP_IF_NO_CERTS();

  TrackNamespace ns({"test"});
  startServer(testPort_, ns, "track1");

  // Give server time to start
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  EXPECT_TRUE(true);  // Server started without crashing

  stopServer();
}

TEST_F(PicoquicIntegrationTest, ClientConnect) {
  SKIP_IF_NO_CERTS();

  TrackNamespace ns({"test"});
  startServer(testPort_, ns, "track1");

  // Give server time to start
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Create and connect client
  auto client = createClient("127.0.0.1", testPort_);
  auto subscriber = std::make_shared<TestSubscriber>();
  std::shared_ptr<MoQSession> session;

  bool connected = connectClient(*client, subscriber, session);

  EXPECT_TRUE(connected);
  EXPECT_NE(session, nullptr);

  if (session) {
    client->close(SessionCloseErrorCode::NO_ERROR);
  }
}

TEST_F(PicoquicIntegrationTest, SubscribeToTrack) {
  SKIP_IF_NO_CERTS();

  TrackNamespace ns({"test-ns"});
  std::string trackName = "test-track";
  startServer(testPort_, ns, trackName);

  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Connect client
  auto client = createClient("127.0.0.1", testPort_);
  auto subscriber = std::make_shared<TestSubscriber>();
  std::shared_ptr<MoQSession> session;

  ASSERT_TRUE(connectClient(*client, subscriber, session));
  ASSERT_NE(session, nullptr);

  // Subscribe to track
  auto consumer = std::make_shared<TestTrackConsumer>();
  std::shared_ptr<Publisher::SubscriptionHandle> handle;
  FullTrackName fullTrackName({ns, trackName});

  bool subscribed = subscribe(*session, fullTrackName, consumer, handle);

  EXPECT_TRUE(subscribed);
  EXPECT_NE(handle, nullptr);

  if (handle) {
    handle->unsubscribe();
  }
  client->close(SessionCloseErrorCode::NO_ERROR);
}

TEST_F(PicoquicIntegrationTest, ReceiveObjects) {
  SKIP_IF_NO_CERTS();

  TrackNamespace ns({"test-ns"});
  std::string trackName = "test-track";
  startServer(testPort_, ns, trackName);

  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Connect client
  auto client = createClient("127.0.0.1", testPort_);
  auto subscriber = std::make_shared<TestSubscriber>();
  std::shared_ptr<MoQSession> session;

  ASSERT_TRUE(connectClient(*client, subscriber, session));

  // Subscribe to track
  auto consumer = std::make_shared<TestTrackConsumer>();
  std::shared_ptr<Publisher::SubscriptionHandle> handle;
  FullTrackName fullTrackName({ns, trackName});

  ASSERT_TRUE(subscribe(*session, fullTrackName, consumer, handle));

  // Wait for subscription to be registered on server
  ASSERT_TRUE(waitFor([this]() {
    return getPublisher()->getSubscriptionCount() > 0;
  }));

  // Server publishes some objects
  getPublisher()->publishObject(0, 0, "Hello");
  getPublisher()->publishObject(0, 1, "World");
  getPublisher()->publishObject(0, 2, "Test");

  // Wait for objects to arrive
  ASSERT_TRUE(consumer->waitForObjects(3, std::chrono::seconds(5)));

  auto objects = consumer->getObjects();
  ASSERT_EQ(objects.size(), 3);

  EXPECT_EQ(objects[0].group, 0);
  EXPECT_EQ(objects[0].object, 0);
  EXPECT_EQ(objects[0].data, "Hello");

  EXPECT_EQ(objects[1].group, 0);
  EXPECT_EQ(objects[1].object, 1);
  EXPECT_EQ(objects[1].data, "World");

  EXPECT_EQ(objects[2].group, 0);
  EXPECT_EQ(objects[2].object, 2);
  EXPECT_EQ(objects[2].data, "Test");

  handle->unsubscribe();
  client->close(SessionCloseErrorCode::NO_ERROR);
}

TEST_F(PicoquicIntegrationTest, MultipleGroups) {
  SKIP_IF_NO_CERTS();

  TrackNamespace ns({"multi-group"});
  std::string trackName = "track";
  startServer(testPort_, ns, trackName);

  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  auto client = createClient("127.0.0.1", testPort_);
  auto subscriber = std::make_shared<TestSubscriber>();
  std::shared_ptr<MoQSession> session;

  ASSERT_TRUE(connectClient(*client, subscriber, session));

  auto consumer = std::make_shared<TestTrackConsumer>();
  std::shared_ptr<Publisher::SubscriptionHandle> handle;
  FullTrackName fullTrackName({ns, trackName});

  ASSERT_TRUE(subscribe(*session, fullTrackName, consumer, handle));
  ASSERT_TRUE(waitFor([this]() {
    return getPublisher()->getSubscriptionCount() > 0;
  }));

  // Publish objects in multiple groups
  getPublisher()->publishObject(0, 0, "G0O0");
  getPublisher()->publishObject(0, 1, "G0O1");
  getPublisher()->publishObject(1, 0, "G1O0");
  getPublisher()->publishObject(1, 1, "G1O1");
  getPublisher()->publishObject(2, 0, "G2O0");

  ASSERT_TRUE(consumer->waitForObjects(5, std::chrono::seconds(5)));

  auto objects = consumer->getObjects();
  ASSERT_EQ(objects.size(), 5);

  // Verify groups
  EXPECT_EQ(objects[0].group, 0);
  EXPECT_EQ(objects[1].group, 0);
  EXPECT_EQ(objects[2].group, 1);
  EXPECT_EQ(objects[3].group, 1);
  EXPECT_EQ(objects[4].group, 2);

  handle->unsubscribe();
  client->close(SessionCloseErrorCode::NO_ERROR);
}

TEST_F(PicoquicIntegrationTest, SubscribeToNonexistentTrack) {
  SKIP_IF_NO_CERTS();

  TrackNamespace ns({"test"});
  startServer(testPort_, ns, "real-track");

  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  auto client = createClient("127.0.0.1", testPort_);
  auto subscriber = std::make_shared<TestSubscriber>();
  std::shared_ptr<MoQSession> session;

  ASSERT_TRUE(connectClient(*client, subscriber, session));

  // Try to subscribe to a different track
  auto consumer = std::make_shared<TestTrackConsumer>();
  std::shared_ptr<Publisher::SubscriptionHandle> handle;
  FullTrackName wrongTrack({ns, "wrong-track"});

  bool subscribed = subscribe(*session, wrongTrack, consumer, handle);

  // Should fail - track doesn't exist
  EXPECT_FALSE(subscribed);
  EXPECT_EQ(handle, nullptr);

  client->close(SessionCloseErrorCode::NO_ERROR);
}

TEST_F(PicoquicIntegrationTest, Unsubscribe) {
  SKIP_IF_NO_CERTS();

  TrackNamespace ns({"test"});
  std::string trackName = "track";
  startServer(testPort_, ns, trackName);

  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  auto client = createClient("127.0.0.1", testPort_);
  auto subscriber = std::make_shared<TestSubscriber>();
  std::shared_ptr<MoQSession> session;

  ASSERT_TRUE(connectClient(*client, subscriber, session));

  auto consumer = std::make_shared<TestTrackConsumer>();
  std::shared_ptr<Publisher::SubscriptionHandle> handle;
  FullTrackName fullTrackName({ns, trackName});

  ASSERT_TRUE(subscribe(*session, fullTrackName, consumer, handle));
  ASSERT_TRUE(waitFor([this]() {
    return getPublisher()->getSubscriptionCount() > 0;
  }));

  // Unsubscribe
  handle->unsubscribe();

  // Publish an object - should not be received
  getPublisher()->publishObject(0, 0, "ShouldNotReceive");

  // Brief wait
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Run executor to process any pending work
  getExecutor()->runFor(std::chrono::milliseconds(100));

  // Verify subscription was removed on server side
  // (after the next publish triggers cleanup)
  getPublisher()->publishObject(0, 1, "TriggerCleanup");
  getExecutor()->runFor(std::chrono::milliseconds(100));

  EXPECT_EQ(getPublisher()->getSubscriptionCount(), 0);

  client->close(SessionCloseErrorCode::NO_ERROR);
}

TEST_F(PicoquicIntegrationTest, MultipleClients) {
  SKIP_IF_NO_CERTS();

  TrackNamespace ns({"multi-client"});
  std::string trackName = "shared-track";
  startServer(testPort_, ns, trackName);

  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Create two clients
  auto client1 = createClient("127.0.0.1", testPort_);
  auto client2 = createClient("127.0.0.1", testPort_);

  auto subscriber1 = std::make_shared<TestSubscriber>();
  auto subscriber2 = std::make_shared<TestSubscriber>();

  std::shared_ptr<MoQSession> session1;
  std::shared_ptr<MoQSession> session2;

  ASSERT_TRUE(connectClient(*client1, subscriber1, session1));
  ASSERT_TRUE(connectClient(*client2, subscriber2, session2));

  // Both subscribe
  auto consumer1 = std::make_shared<TestTrackConsumer>();
  auto consumer2 = std::make_shared<TestTrackConsumer>();

  std::shared_ptr<Publisher::SubscriptionHandle> handle1;
  std::shared_ptr<Publisher::SubscriptionHandle> handle2;

  FullTrackName fullTrackName({ns, trackName});

  ASSERT_TRUE(subscribe(*session1, fullTrackName, consumer1, handle1));
  ASSERT_TRUE(subscribe(*session2, fullTrackName, consumer2, handle2));

  ASSERT_TRUE(waitFor([this]() {
    return getPublisher()->getSubscriptionCount() >= 2;
  }));

  // Publish an object
  getPublisher()->publishObject(0, 0, "SharedData");

  // Both should receive it
  ASSERT_TRUE(consumer1->waitForObjects(1, std::chrono::seconds(5)));
  ASSERT_TRUE(consumer2->waitForObjects(1, std::chrono::seconds(5)));

  EXPECT_EQ(consumer1->getObjects()[0].data, "SharedData");
  EXPECT_EQ(consumer2->getObjects()[0].data, "SharedData");

  handle1->unsubscribe();
  handle2->unsubscribe();
  client1->close(SessionCloseErrorCode::NO_ERROR);
  client2->close(SessionCloseErrorCode::NO_ERROR);
}

TEST_F(PicoquicIntegrationTest, LargePayload) {
  SKIP_IF_NO_CERTS();

  TrackNamespace ns({"large"});
  std::string trackName = "track";
  startServer(testPort_, ns, trackName);

  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  auto client = createClient("127.0.0.1", testPort_);
  auto subscriber = std::make_shared<TestSubscriber>();
  std::shared_ptr<MoQSession> session;

  ASSERT_TRUE(connectClient(*client, subscriber, session));

  auto consumer = std::make_shared<TestTrackConsumer>();
  std::shared_ptr<Publisher::SubscriptionHandle> handle;
  FullTrackName fullTrackName({ns, trackName});

  ASSERT_TRUE(subscribe(*session, fullTrackName, consumer, handle));
  ASSERT_TRUE(waitFor([this]() {
    return getPublisher()->getSubscriptionCount() > 0;
  }));

  // Create a large payload (10KB)
  std::string largeData(10 * 1024, 'X');
  getPublisher()->publishObject(0, 0, largeData);

  ASSERT_TRUE(consumer->waitForObjects(1, std::chrono::seconds(10)));

  auto objects = consumer->getObjects();
  ASSERT_EQ(objects.size(), 1);
  EXPECT_EQ(objects[0].data.size(), largeData.size());
  EXPECT_EQ(objects[0].data, largeData);

  handle->unsubscribe();
  client->close(SessionCloseErrorCode::NO_ERROR);
}

TEST_F(PicoquicIntegrationTest, RapidPublish) {
  SKIP_IF_NO_CERTS();

  TrackNamespace ns({"rapid"});
  std::string trackName = "track";
  startServer(testPort_, ns, trackName);

  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  auto client = createClient("127.0.0.1", testPort_);
  auto subscriber = std::make_shared<TestSubscriber>();
  std::shared_ptr<MoQSession> session;

  ASSERT_TRUE(connectClient(*client, subscriber, session));

  auto consumer = std::make_shared<TestTrackConsumer>();
  std::shared_ptr<Publisher::SubscriptionHandle> handle;
  FullTrackName fullTrackName({ns, trackName});

  ASSERT_TRUE(subscribe(*session, fullTrackName, consumer, handle));
  ASSERT_TRUE(waitFor([this]() {
    return getPublisher()->getSubscriptionCount() > 0;
  }));

  // Rapidly publish 100 objects
  constexpr int numObjects = 100;
  for (int i = 0; i < numObjects; i++) {
    getPublisher()->publishObject(0, i, "Object" + std::to_string(i));
  }

  // Wait for all objects
  ASSERT_TRUE(consumer->waitForObjects(numObjects, std::chrono::seconds(10)));

  auto objects = consumer->getObjects();
  EXPECT_EQ(objects.size(), numObjects);

  // Verify ordering
  for (int i = 0; i < numObjects; i++) {
    EXPECT_EQ(objects[i].object, i);
    EXPECT_EQ(objects[i].data, "Object" + std::to_string(i));
  }

  handle->unsubscribe();
  client->close(SessionCloseErrorCode::NO_ERROR);
}

TEST_F(PicoquicIntegrationTest, ClientDisconnect) {
  SKIP_IF_NO_CERTS();

  TrackNamespace ns({"disconnect"});
  std::string trackName = "track";
  startServer(testPort_, ns, trackName);

  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  auto client = createClient("127.0.0.1", testPort_);
  auto subscriber = std::make_shared<TestSubscriber>();
  std::shared_ptr<MoQSession> session;

  ASSERT_TRUE(connectClient(*client, subscriber, session));

  auto consumer = std::make_shared<TestTrackConsumer>();
  std::shared_ptr<Publisher::SubscriptionHandle> handle;
  FullTrackName fullTrackName({ns, trackName});

  ASSERT_TRUE(subscribe(*session, fullTrackName, consumer, handle));
  ASSERT_TRUE(waitFor([this]() {
    return getPublisher()->getSubscriptionCount() > 0;
  }));

  // Close client
  client->close(SessionCloseErrorCode::NO_ERROR);

  // Server should handle disconnect gracefully
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  getExecutor()->runFor(std::chrono::milliseconds(100));

  // Publish should not crash even with no subscribers
  getPublisher()->publishObject(0, 0, "NoOneListening");

  EXPECT_TRUE(true);  // Test passes if no crash
}

} // namespace moxygen::test::integration

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

#else // !MOXYGEN_QUIC_PICOQUIC

// Placeholder when picoquic is not available
#include <gtest/gtest.h>

TEST(PicoquicIntegrationTest, SkipWhenPicoquicNotAvailable) {
  GTEST_SKIP() << "Picoquic transport not available";
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

#endif // MOXYGEN_QUIC_PICOQUIC
