/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <thread>

#include <folly/portability/GMock.h>
#include <moxygen/MoQSession.h>
#include <moxygen/events/MoQFollyExecutorImpl.h>

namespace moxygen::test {

// Mock MoQSession for testing relay behavior
class MockMoQSession : public MoQSession {
 public:
  // Constructor that accepts an external executor
  explicit MockMoQSession(std::shared_ptr<MoQExecutor> exec)
      : MoQSession(
            folly::MaybeManagedPtr<proxygen::WebTransport>(nullptr),
            exec),
        ownsEventBase_(false) {
    setSetupDefaults();
  }

  // Default constructor that creates and manages its own executor
  MockMoQSession()
      : MoQSession(
            folly::MaybeManagedPtr<proxygen::WebTransport>(nullptr),
            std::make_shared<MoQFollyExecutorImpl>(&evb_)),
        ownsEventBase_(true) {
    setSetupDefaults();
    // Start EventBase in background thread for async operations
    evbThread_ = std::thread([this]() { evb_.loopForever(); });
  }

  ~MockMoQSession() {
    if (ownsEventBase_) {
      // Stop EventBase and wait for thread
      evb_.terminateLoopSoon();
      if (evbThread_.joinable()) {
        evbThread_.join();
      }
    }
  }

  MOCK_METHOD(
      folly::coro::Task<PublishNamespaceResult>,
      publishNamespace,
      (PublishNamespace, std::shared_ptr<Subscriber::PublishNamespaceCallback>),
      (override));

  MOCK_METHOD(
      PublishResult,
      publish,
      (PublishRequest, std::shared_ptr<Publisher::SubscriptionHandle>),
      (override));

  MOCK_METHOD(
      folly::coro::Task<SubscribeResult>,
      subscribe,
      (SubscribeRequest, std::shared_ptr<TrackConsumer>),
      (override));

  MOCK_METHOD(
      folly::coro::Task<Publisher::FetchResult>,
      fetch,
      (Fetch, std::shared_ptr<FetchConsumer>),
      (override));

  MOCK_METHOD(
      std::optional<uint64_t>,
      getNegotiatedVersion,
      (),
      (const, override));

  MOCK_METHOD(
      const std::optional<SetupParameters>&,
      getPeerSetupParams,
      (),
      (const, override));

  MOCK_METHOD(
      const MessageParameterExtensions&,
      getMessageParameterExtensions,
      (),
      (const, override));

  // What the getters above return unless a test overrides them.
  std::optional<SetupParameters> peerSetupParams;
  MessageParameterExtensions messageParameterExtensions;

  MOCK_METHOD(
      folly::coro::Task<Publisher::TrackStatusResult>,
      trackStatus,
      (TrackStatus),
      (override));

 private:
  void setSetupDefaults() {
    ON_CALL(*this, getPeerSetupParams())
        .WillByDefault(testing::ReturnRef(peerSetupParams));
    ON_CALL(*this, getMessageParameterExtensions())
        .WillByDefault(testing::ReturnRef(messageParameterExtensions));
  }

  folly::EventBase evb_;
  std::thread evbThread_;
  bool ownsEventBase_;
};

} // namespace moxygen::test
