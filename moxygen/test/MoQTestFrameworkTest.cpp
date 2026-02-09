/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// Tests for the compatibility test framework
// Verifies TestExecutor, MockMoQTransport, and MoQTestFixtureCompat work correctly

#include <moxygen/test/MoQTestFixtureCompat.h>
#include <moxygen/test/MockMoQTransport.h>
#include <moxygen/test/TestExecutor.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>

namespace moxygen::test {

// Test TestExecutor basic functionality
class TestExecutorTest : public ::testing::Test {
 protected:
  TestExecutor executor_;
};

TEST_F(TestExecutorTest, ScheduleAndRun) {
  std::atomic<bool> executed{false};

  executor_.schedule([&executed]() { executed = true; });

  // Run until executed or timeout
  bool completed = executor_.runUntil(
      [&executed]() { return executed.load(); },
      std::chrono::milliseconds(100));

  EXPECT_TRUE(completed);
  EXPECT_TRUE(executed);
}

TEST_F(TestExecutorTest, ScheduleMultiple) {
  std::atomic<int> counter{0};

  executor_.schedule([&counter]() { counter++; });
  executor_.schedule([&counter]() { counter++; });
  executor_.schedule([&counter]() { counter++; });

  bool completed = executor_.runUntil(
      [&counter]() { return counter.load() >= 3; },
      std::chrono::milliseconds(100));

  EXPECT_TRUE(completed);
  EXPECT_EQ(counter, 3);
}

TEST_F(TestExecutorTest, ScheduleAfterDelay) {
  std::atomic<bool> executed{false};
  auto start = std::chrono::steady_clock::now();

  executor_.scheduleAfter(
      [&executed]() { executed = true; },
      std::chrono::milliseconds(50));

  bool completed = executor_.runUntil(
      [&executed]() { return executed.load(); },
      std::chrono::milliseconds(200));

  auto elapsed = std::chrono::steady_clock::now() - start;

  EXPECT_TRUE(completed);
  EXPECT_TRUE(executed);
  // Should have waited at least 50ms
  EXPECT_GE(
      std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(),
      50);
}

TEST_F(TestExecutorTest, RunForDuration) {
  auto start = std::chrono::steady_clock::now();

  executor_.runFor(std::chrono::milliseconds(100));

  auto elapsed = std::chrono::steady_clock::now() - start;

  // Should have run for approximately 100ms
  EXPECT_GE(
      std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(),
      100);
}

TEST_F(TestExecutorTest, TimeoutOnCondition) {
  // Condition that never becomes true
  bool completed = executor_.runUntil(
      []() { return false; },
      std::chrono::milliseconds(50));

  EXPECT_FALSE(completed);
}

// Test MockMoQTransport
class MockMoQTransportTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto [client, server] = createLinkedTransports();
    clientTransport_ = std::move(client);
    serverTransport_ = std::move(server);
  }

  std::unique_ptr<MockMoQTransport> clientTransport_;
  std::unique_ptr<MockMoQTransport> serverTransport_;
};

TEST_F(MockMoQTransportTest, LinkedTransports) {
  EXPECT_EQ(clientTransport_->getPeer(), serverTransport_.get());
  EXPECT_EQ(serverTransport_->getPeer(), clientTransport_.get());
}

TEST_F(MockMoQTransportTest, ControlStreams) {
  EXPECT_NE(clientTransport_->getControlStream(), nullptr);
  EXPECT_NE(serverTransport_->getControlStream(), nullptr);
}

TEST_F(MockMoQTransportTest, StreamWrite) {
  auto stream = std::make_shared<MockStreamHandle>(100);

  std::string data = "Hello, World!";
  stream->write(Buffer::copyBuffer(data.data(), data.size()));

  EXPECT_TRUE(stream->hasData());

  auto readData = stream->read();
  ASSERT_NE(readData, nullptr);
  EXPECT_EQ(readData->computeChainDataLength(), data.size());
}

TEST_F(MockMoQTransportTest, SimulateControlData) {
  std::string data = "test control data";
  clientTransport_->simulateControlData(
      Buffer::copyBuffer(data.data(), data.size()));

  auto received = clientTransport_->getIncomingControlData();
  ASSERT_NE(received, nullptr);
  EXPECT_EQ(received->computeChainDataLength(), data.size());
}

TEST_F(MockMoQTransportTest, SimulateIncomingStream) {
  auto stream = std::make_shared<MockStreamHandle>(42);
  clientTransport_->simulateIncomingUniStream(stream);

  auto received = clientTransport_->getNextIncomingUniStream();
  ASSERT_NE(received, nullptr);
  EXPECT_EQ(received->getStreamId(), 42);

  // Second call should return nullptr
  auto second = clientTransport_->getNextIncomingUniStream();
  EXPECT_EQ(second, nullptr);
}

// Test AsyncTestHelper
class AsyncTestHelperTest : public ::testing::Test {
 protected:
  TestExecutor executor_;
};

TEST_F(AsyncTestHelperTest, BasicCompletion) {
  AsyncTestHelper async;

  executor_.schedule([&async]() { async.done(); });

  EXPECT_TRUE(async.wait(executor_, std::chrono::milliseconds(100)));
  EXPECT_TRUE(async.isCompleted());
  EXPECT_FALSE(async.isFailed());
}

TEST_F(AsyncTestHelperTest, FailureCase) {
  AsyncTestHelper async;

  executor_.schedule([&async]() { async.fail("test failure"); });

  EXPECT_TRUE(async.wait(executor_, std::chrono::milliseconds(100)));
  EXPECT_TRUE(async.isCompleted());
  EXPECT_TRUE(async.isFailed());
  EXPECT_EQ(async.getFailReason(), "test failure");
}

TEST_F(AsyncTestHelperTest, Timeout) {
  AsyncTestHelper async;

  // Don't call done()
  EXPECT_FALSE(async.wait(executor_, std::chrono::milliseconds(50)));
  EXPECT_FALSE(async.isCompleted());
}

// Test MoQTestFixtureCompat
class MoQTestFixtureCompatTest : public MoQTestFixtureCompat {};

TEST_P(MoQTestFixtureCompatTest, VersionIsSet) {
  EXPECT_EQ(getVersion(), kVersionDraftCurrent);
}

TEST_P(MoQTestFixtureCompatTest, TransportsLinked) {
  EXPECT_EQ(getClientTransport().getPeer(), &getServerTransport());
  EXPECT_EQ(getServerTransport().getPeer(), &getClientTransport());
}

TEST_P(MoQTestFixtureCompatTest, CreateClientSetup) {
  auto buf = createClientSetup();
  ASSERT_NE(buf, nullptr);
  EXPECT_GT(buf->computeChainDataLength(), 0);
}

TEST_P(MoQTestFixtureCompatTest, CreateServerSetup) {
  auto buf = createServerSetup();
  ASSERT_NE(buf, nullptr);
  EXPECT_GT(buf->computeChainDataLength(), 0);
}

TEST_P(MoQTestFixtureCompatTest, CreateSubscribe) {
  FullTrackName trackName({TrackNamespace({"test", "ns"}), "track"});
  auto buf = createSubscribe(trackName);
  ASSERT_NE(buf, nullptr);
  EXPECT_GT(buf->computeChainDataLength(), 0);
}

TEST_P(MoQTestFixtureCompatTest, WaitForCondition) {
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

TEST_P(MoQTestFixtureCompatTest, RunAsyncTest) {
  bool result = runAsyncTest([this](AsyncTestHelper& async) {
    getExecutor().schedule([&async]() { async.done(); });
  });

  EXPECT_TRUE(result);
}

INSTANTIATE_COMPAT_TEST_SUITE(MoQTestFixtureCompatTest);

} // namespace moxygen::test

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
