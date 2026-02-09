/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// Callback-based session tests for std-mode (no Folly coroutines)
// Tests core MoQ session functionality using the compat test framework.

#include <moxygen/test/MoQTestFixtureCompat.h>
#include <moxygen/test/MockMoQTransport.h>
#include <moxygen/test/TestExecutor.h>

#include <moxygen/MoQFramer.h>
#include <moxygen/MoQTypes.h>
#include <moxygen/MoQVersions.h>

#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <atomic>
#include <chrono>

namespace moxygen::test {

// Test constants
static const FullTrackName kTestTrackName{TrackNamespace{{"test"}}, "track"};

/**
 * Test fixture for callback-based session tests.
 * Uses mock transports to simulate client/server communication.
 */
class MoQSessionTestCompat : public MoQTestFixtureCompat {};

// ============================================================================
// Frame Writing Tests
// ============================================================================

TEST_P(MoQSessionTestCompat, ClientSetupFrameRoundTrip) {
  auto buf = createClientSetup();
  ASSERT_NE(buf, nullptr);
  EXPECT_GT(buf->computeChainDataLength(), 0);
}

TEST_P(MoQSessionTestCompat, ServerSetupFrameRoundTrip) {
  auto buf = createServerSetup();
  ASSERT_NE(buf, nullptr);
  EXPECT_GT(buf->computeChainDataLength(), 0);
}

TEST_P(MoQSessionTestCompat, SubscribeFrameRoundTrip) {
  auto buf = createSubscribe(kTestTrackName);
  ASSERT_NE(buf, nullptr);
  EXPECT_GT(buf->computeChainDataLength(), 0);
}

TEST_P(MoQSessionTestCompat, SubscribeOkFrameRoundTrip) {
  auto buf = createSubscribeOk(RequestID(1), TrackAlias(1));
  ASSERT_NE(buf, nullptr);
  EXPECT_GT(buf->computeChainDataLength(), 0);
}

// ============================================================================
// Mock Transport Tests
// ============================================================================

TEST_P(MoQSessionTestCompat, MockTransportsLinked) {
  EXPECT_EQ(&getClientTransport(), getServerTransport().getPeer());
  EXPECT_EQ(&getServerTransport(), getClientTransport().getPeer());
}

TEST_P(MoQSessionTestCompat, MockStreamWrite) {
  auto stream = std::make_shared<MockStreamHandle>(100);

  std::string data = "test data";
  stream->write(Buffer::copyBuffer(data.data(), data.size()));

  EXPECT_TRUE(stream->hasData());

  auto readData = stream->read();
  ASSERT_NE(readData, nullptr);
  EXPECT_EQ(readData->computeChainDataLength(), data.size());
}

TEST_P(MoQSessionTestCompat, SimulateControlData) {
  auto setupBuf = createClientSetup();
  ASSERT_NE(setupBuf, nullptr);

  getClientTransport().simulateControlData(std::move(setupBuf));

  auto received = getClientTransport().getIncomingControlData();
  ASSERT_NE(received, nullptr);
  EXPECT_GT(received->computeChainDataLength(), 0);
}

TEST_P(MoQSessionTestCompat, SimulateIncomingStream) {
  auto stream = std::make_shared<MockStreamHandle>(42);
  getClientTransport().simulateIncomingUniStream(stream);

  auto received = getClientTransport().getNextIncomingUniStream();
  ASSERT_NE(received, nullptr);
  EXPECT_EQ(received->getStreamId(), 42);

  // Second call should return nullptr
  auto second = getClientTransport().getNextIncomingUniStream();
  EXPECT_EQ(second, nullptr);
}

// ============================================================================
// Async Test Helper Tests
// ============================================================================

TEST_P(MoQSessionTestCompat, AsyncTestHelperDone) {
  AsyncTestHelper async;

  getExecutor().schedule([&async]() { async.done(); });

  EXPECT_TRUE(async.wait(getExecutor(), std::chrono::milliseconds(100)));
  EXPECT_TRUE(async.isCompleted());
  EXPECT_FALSE(async.isFailed());
}

TEST_P(MoQSessionTestCompat, AsyncTestHelperFail) {
  AsyncTestHelper async;

  getExecutor().schedule([&async]() { async.fail("test failure"); });

  EXPECT_TRUE(async.wait(getExecutor(), std::chrono::milliseconds(100)));
  EXPECT_TRUE(async.isCompleted());
  EXPECT_TRUE(async.isFailed());
  EXPECT_EQ(async.getFailReason(), "test failure");
}

TEST_P(MoQSessionTestCompat, AsyncTestHelperTimeout) {
  AsyncTestHelper async;

  // Don't call done()
  EXPECT_FALSE(async.wait(getExecutor(), std::chrono::milliseconds(50)));
  EXPECT_FALSE(async.isCompleted());
}

// ============================================================================
// Scheduled Callback Tests
// ============================================================================

TEST_P(MoQSessionTestCompat, ScheduleCallback) {
  std::atomic<bool> executed{false};

  getExecutor().schedule([&executed]() { executed = true; });

  bool completed = waitFor(
      [&executed]() { return executed.load(); },
      std::chrono::milliseconds(100));

  EXPECT_TRUE(completed);
  EXPECT_TRUE(executed);
}

TEST_P(MoQSessionTestCompat, ScheduleAfterDelay) {
  std::atomic<bool> executed{false};
  auto start = std::chrono::steady_clock::now();

  getExecutor().scheduleAfter(
      [&executed]() { executed = true; },
      std::chrono::milliseconds(30));

  bool completed = waitFor(
      [&executed]() { return executed.load(); },
      std::chrono::milliseconds(200));

  auto elapsed = std::chrono::steady_clock::now() - start;

  EXPECT_TRUE(completed);
  EXPECT_TRUE(executed);
  EXPECT_GE(
      std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(),
      30);
}

TEST_P(MoQSessionTestCompat, MultipleCallbacks) {
  std::atomic<int> counter{0};

  getExecutor().schedule([&counter]() { counter++; });
  getExecutor().schedule([&counter]() { counter++; });
  getExecutor().schedule([&counter]() { counter++; });

  bool completed = waitFor(
      [&counter]() { return counter.load() >= 3; },
      std::chrono::milliseconds(100));

  EXPECT_TRUE(completed);
  EXPECT_EQ(counter, 3);
}

TEST_P(MoQSessionTestCompat, NestedCallbacks) {
  std::atomic<int> counter{0};

  getExecutor().schedule([this, &counter]() {
    counter++;
    getExecutor().schedule([&counter]() {
      counter++;
    });
  });

  bool completed = waitFor(
      [&counter]() { return counter.load() >= 2; },
      std::chrono::milliseconds(100));

  EXPECT_TRUE(completed);
  EXPECT_EQ(counter, 2);
}

// ============================================================================
// RunAsyncTest Helper Tests
// ============================================================================

TEST_P(MoQSessionTestCompat, RunAsyncTestSuccess) {
  bool result = runAsyncTest([this](AsyncTestHelper& async) {
    getExecutor().schedule([&async]() { async.done(); });
  });

  EXPECT_TRUE(result);
}

TEST_P(MoQSessionTestCompat, RunAsyncTestWithDelay) {
  bool result = runAsyncTest([this](AsyncTestHelper& async) {
    getExecutor().scheduleAfter(
        [&async]() { async.done(); },
        std::chrono::milliseconds(20));
  });

  EXPECT_TRUE(result);
}

TEST_P(MoQSessionTestCompat, RunAsyncTestTimeout) {
  bool result = runAsyncTest(
      [](AsyncTestHelper& /*async*/) {
        // Don't call done() or fail()
      },
      std::chrono::milliseconds(50));

  EXPECT_FALSE(result);
}

// ============================================================================
// Frame Builder Tests
// ============================================================================

TEST_P(MoQSessionTestCompat, CreateSubscribeWithTrackName) {
  FullTrackName trackName({TrackNamespace({"ns1", "ns2"}), "trackname"});
  auto buf = createSubscribe(trackName);
  ASSERT_NE(buf, nullptr);
  EXPECT_GT(buf->computeChainDataLength(), 0);
}

TEST_P(MoQSessionTestCompat, CreateMultipleFrames) {
  auto clientSetup = createClientSetup();
  auto serverSetup = createServerSetup();
  auto subscribe = createSubscribe(kTestTrackName);
  auto subscribeOk = createSubscribeOk(RequestID(1), TrackAlias(1));

  EXPECT_NE(clientSetup, nullptr);
  EXPECT_NE(serverSetup, nullptr);
  EXPECT_NE(subscribe, nullptr);
  EXPECT_NE(subscribeOk, nullptr);
}

TEST_P(MoQSessionTestCompat, CreateSubscribeWithDifferentVersions) {
  // Verify frame creation works with the test version
  EXPECT_EQ(getVersion(), kVersionDraftCurrent);
  auto buf = createSubscribe(kTestTrackName);
  ASSERT_NE(buf, nullptr);
}

// ============================================================================
// WaitFor Tests
// ============================================================================

TEST_P(MoQSessionTestCompat, WaitForSuccess) {
  std::atomic<bool> ready{false};

  getExecutor().scheduleAfter(
      [&ready]() { ready = true; },
      std::chrono::milliseconds(10));

  bool result = waitFor(
      [&ready]() { return ready.load(); },
      std::chrono::milliseconds(100));

  EXPECT_TRUE(result);
  EXPECT_TRUE(ready);
}

TEST_P(MoQSessionTestCompat, WaitForTimeout) {
  std::atomic<bool> ready{false};
  // Don't schedule anything

  bool result = waitFor(
      [&ready]() { return ready.load(); },
      std::chrono::milliseconds(50));

  EXPECT_FALSE(result);
  EXPECT_FALSE(ready);
}

TEST_P(MoQSessionTestCompat, WaitForImmediateCondition) {
  bool result = waitFor(
      []() { return true; },
      std::chrono::milliseconds(100));

  EXPECT_TRUE(result);
}

// ============================================================================
// Stream ID Tests
// ============================================================================

TEST_P(MoQSessionTestCompat, StreamIdsUnique) {
  auto stream1 = std::make_shared<MockStreamHandle>(1);
  auto stream2 = std::make_shared<MockStreamHandle>(2);
  auto stream3 = std::make_shared<MockStreamHandle>(3);

  EXPECT_EQ(stream1->getStreamId(), 1);
  EXPECT_EQ(stream2->getStreamId(), 2);
  EXPECT_EQ(stream3->getStreamId(), 3);
}

TEST_P(MoQSessionTestCompat, MultipleStreamSimulation) {
  auto stream1 = std::make_shared<MockStreamHandle>(10);
  auto stream2 = std::make_shared<MockStreamHandle>(20);

  getClientTransport().simulateIncomingUniStream(stream1);
  getClientTransport().simulateIncomingUniStream(stream2);

  auto received1 = getClientTransport().getNextIncomingUniStream();
  auto received2 = getClientTransport().getNextIncomingUniStream();

  ASSERT_NE(received1, nullptr);
  ASSERT_NE(received2, nullptr);
  EXPECT_EQ(received1->getStreamId(), 10);
  EXPECT_EQ(received2->getStreamId(), 20);
}

INSTANTIATE_COMPAT_TEST_SUITE(MoQSessionTestCompat);

} // namespace moxygen::test

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
