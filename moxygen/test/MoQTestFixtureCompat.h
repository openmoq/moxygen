/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

// Compatibility test fixture that works in all build modes
// Provides a simplified test infrastructure for callback-based testing

#include <moxygen/MoQCodec.h>
#include <moxygen/MoQFramer.h>
#include <moxygen/MoQTypes.h>
#include <moxygen/MoQVersions.h>
#include <moxygen/test/MockMoQTransport.h>
#include <moxygen/test/TestExecutor.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>

namespace moxygen::test {

// Default test timeout
constexpr auto kDefaultTestTimeout = std::chrono::seconds(5);

/**
 * Callback-based async test helper
 *
 * Usage:
 *   AsyncTestHelper async;
 *   someAsyncOperation([&async](Result r) {
 *     EXPECT_TRUE(r.isSuccess());
 *     async.done();
 *   });
 *   ASSERT_TRUE(async.wait(executor));
 */
class AsyncTestHelper {
 public:
  void done() {
    completed_ = true;
  }

  void fail(const std::string& reason = "") {
    failed_ = true;
    failReason_ = reason;
    completed_ = true;
  }

  bool wait(
      TestExecutor& executor,
      std::chrono::milliseconds timeout = kDefaultTestTimeout) {
    return executor.runUntil(
        [this]() { return completed_.load(); }, timeout);
  }

  bool isCompleted() const {
    return completed_;
  }

  bool isFailed() const {
    return failed_;
  }

  const std::string& getFailReason() const {
    return failReason_;
  }

 private:
  std::atomic<bool> completed_{false};
  std::atomic<bool> failed_{false};
  std::string failReason_;
};

/**
 * Version parameters for parametrized tests
 */
struct VersionParamsCompat {
  uint64_t version;

  explicit VersionParamsCompat(uint64_t v) : version(v) {}
};

/**
 * Base test fixture for MoQ compatibility tests
 *
 * This fixture provides:
 * - Test executor for async operations
 * - Mock transports for client/server simulation
 * - Frame writer/parser for message creation
 * - Helper methods for common test operations
 */
class MoQTestFixtureCompat
    : public ::testing::TestWithParam<VersionParamsCompat> {
 public:
  void SetUp() override {
    version_ = GetParam().version;
    frameWriter_.initializeVersion(version_);
    frameParser_.initializeVersion(version_);

    // Create linked transports
    auto [client, server] = createLinkedTransports();
    clientTransport_ = std::move(client);
    serverTransport_ = std::move(server);
  }

  void TearDown() override {
    executor_.stop();
  }

 protected:
  // Get the test version
  uint64_t getVersion() const {
    return version_;
  }

  // Get the executor
  TestExecutor& getExecutor() {
    return executor_;
  }

  // Get transports
  MockMoQTransport& getClientTransport() {
    return *clientTransport_;
  }

  MockMoQTransport& getServerTransport() {
    return *serverTransport_;
  }

  // Helper to create a ClientSetup message
  std::unique_ptr<Buffer> createClientSetup(
      const std::vector<uint64_t>& versions = {}) {
    ClientSetup setup;
    setup.supportedVersions = versions.empty()
        ? std::vector<uint64_t>{version_}
        : versions;
    setup.params.insertParam(
        Parameter(compat::to_underlying(SetupKey::PATH), "/test"));
    setup.params.insertParam(
        Parameter(compat::to_underlying(SetupKey::MAX_REQUEST_ID), 100));

    BufQueue buf;
    auto result = writeClientSetup(buf, setup, version_);
    if (!result.hasValue()) {
      return nullptr;
    }
    return buf.move();
  }

  // Helper to create a ServerSetup message
  std::unique_ptr<Buffer> createServerSetup() {
    ServerSetup setup;
    setup.selectedVersion = version_;

    BufQueue buf;
    auto result = writeServerSetup(buf, setup, version_);
    if (!result.hasValue()) {
      return nullptr;
    }
    return buf.move();
  }

  // Helper to create a Subscribe message
  std::unique_ptr<Buffer> createSubscribe(
      const FullTrackName& trackName,
      uint8_t priority = 128) {
    auto req = SubscribeRequest::make(
        trackName,
        priority,
        GroupOrder::OldestFirst,
        true,
        LocationType::LargestObject,
        std::nullopt,
        0,
        {});

    BufQueue buf;
    auto result = frameWriter_.writeSubscribeRequest(buf, req);
    if (!result.hasValue()) {
      return nullptr;
    }
    return buf.move();
  }

  // Helper to create a SubscribeOk message
  std::unique_ptr<Buffer> createSubscribeOk(
      RequestID requestId,
      TrackAlias alias) {
    SubscribeOk ok;
    ok.requestID = requestId;
    ok.trackAlias = alias;
    ok.expires = std::chrono::milliseconds(0);
    ok.groupOrder = GroupOrder::OldestFirst;

    BufQueue buf;
    auto result = frameWriter_.writeSubscribeOk(buf, ok);
    if (!result.hasValue()) {
      return nullptr;
    }
    return buf.move();
  }

  // Helper to run async test with timeout
  template <typename Fn>
  bool runAsyncTest(Fn&& fn, std::chrono::milliseconds timeout = kDefaultTestTimeout) {
    AsyncTestHelper async;
    fn(async);
    return async.wait(executor_, timeout);
  }

  // Wait for a condition with timeout
  template <typename Pred>
  bool waitFor(Pred pred, std::chrono::milliseconds timeout = kDefaultTestTimeout) {
    return executor_.runUntil(pred, timeout);
  }

  uint64_t version_{kVersionDraftCurrent};
  TestExecutor executor_;
  MoQFrameWriter frameWriter_;
  MoQFrameParser frameParser_;
  std::unique_ptr<MockMoQTransport> clientTransport_;
  std::unique_ptr<MockMoQTransport> serverTransport_;
};

// Helper macro for parametrized compat tests
#define INSTANTIATE_COMPAT_TEST_SUITE(TestSuite)          \
  INSTANTIATE_TEST_SUITE_P(                               \
      TestSuite,                                          \
      TestSuite,                                          \
      ::testing::Values(VersionParamsCompat(kVersionDraftCurrent)))

} // namespace moxygen::test
