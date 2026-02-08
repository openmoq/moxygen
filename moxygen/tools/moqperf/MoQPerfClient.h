/*
 *  Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 *  This source code is licensed under the MIT license found in the LICENSE
 *  file in the root directory of this source tree.
 *
 */

#pragma once

#include <moxygen/MoQClient.h>
#include <moxygen/Subscriber.h>
#include <moxygen/events/MoQFollyExecutorImpl.h>
#include <moxygen/tools/moqperf/MoQPerfParams.h>

#include <utility>

namespace moxygen {

class MoQPerfClientSubgroupConsumer : public SubgroupConsumer {
 public:
  explicit MoQPerfClientSubgroupConsumer(
      std::function<void(uint64_t)> dataSentFn)
      : SubgroupConsumer(), dataSentFn_(std::move(dataSentFn)) {}

  compat::Expected<compat::Unit, MoQPublishError> object(
      uint64_t /* objectID */,
      Payload payload,
      Extensions /* extensions */,
      bool /* finSubgroup */) override {
    dataSentFn_(payload->computeChainDataLength());
    return compat::unit;
  }

  compat::Expected<compat::Unit, MoQPublishError> beginObject(
      uint64_t /* objectID */,
      uint64_t /* length */,
      Payload initialPayload,
      Extensions /* extensions */) override {
    dataSentFn_(initialPayload->computeChainDataLength());
    return compat::unit;
  }

  folly::Expected<ObjectPublishStatus, MoQPublishError> objectPayload(
      Payload payload,
      bool /* finSubgroup */) override {
    dataSentFn_(payload->computeChainDataLength());
    return ObjectPublishStatus::DONE;
  }

  compat::Expected<compat::Unit, MoQPublishError> endOfGroup(
      uint64_t /* endOfGroupObjectID */) override {
    return compat::unit;
  }

  compat::Expected<compat::Unit, MoQPublishError> endOfTrackAndGroup(
      uint64_t /* endOfTrackObjectID */) override {
    return compat::unit;
  }

  compat::Expected<compat::Unit, MoQPublishError> endOfSubgroup() override {
    return compat::unit;
  }

  void reset(ResetStreamErrorCode /* error */) override {}

 private:
  std::function<void(uint64_t)> dataSentFn_;
};

class MoQPerfClientTrackConsumer : public TrackConsumer {
 public:
  compat::Expected<compat::Unit, MoQPublishError> setTrackAlias(
      TrackAlias) override {
    return compat::unit;
  }

  folly::Expected<std::shared_ptr<SubgroupConsumer>, MoQPublishError>
  beginSubgroup(
      uint64_t /* groupID */,
      uint64_t /* subgroupID */,
      Priority /* priority */,
      bool /* containsLastInGroup */ = false) override {
    return std::make_shared<MoQPerfClientSubgroupConsumer>(
        [this](uint64_t inc) { dataSent_ += inc; });
  }

  folly::Expected<folly::SemiFuture<folly::Unit>, MoQPublishError>
  awaitStreamCredit() override {
    return compat::unit;
  }

  compat::Expected<compat::Unit, MoQPublishError> objectStream(
      const ObjectHeader& /* header */,
      Payload payload,
      bool /* lastInGroup */ = false) override {
    dataSent_ += payload->computeChainDataLength();
    return compat::unit;
  }

  compat::Expected<compat::Unit, MoQPublishError> datagram(
      const ObjectHeader& /* header */,
      Payload payload,
      bool /* lastInGroup */ = false) override {
    dataSent_ += payload->computeChainDataLength();
    return compat::unit;
  }

  compat::Expected<compat::Unit, MoQPublishError> publishDone(
      PublishDone /* pubDone */) override {
    return compat::unit;
  }

  uint64_t getDataSent() {
    return dataSent_;
  }

 private:
  uint64_t dataSent_{0};
};

class MoQPerfClientFetchConsumer : public FetchConsumer {
 public:
  compat::Expected<compat::Unit, MoQPublishError> object(
      uint64_t groupID,
      uint64_t subgroupID,
      uint64_t objectID,
      Payload payload,
      Extensions extensions = noExtensions(),
      bool finFetch = false);

  void incrementFetchDataSent(uint64_t amount);

  uint64_t getFetchDataSent();

  virtual void checkpoint() override;

  virtual compat::Expected<compat::Unit, MoQPublishError> beginObject(
      uint64_t groupID,
      uint64_t subgroupID,
      uint64_t objectID,
      uint64_t length,
      Payload initialPayload,
      Extensions extensions = noExtensions()) override;

  virtual folly::Expected<ObjectPublishStatus, MoQPublishError> objectPayload(
      Payload payload,
      bool finSubgroup = false) override;

  virtual compat::Expected<compat::Unit, MoQPublishError> endOfGroup(
      uint64_t groupID,
      uint64_t subgroupID,
      uint64_t objectID,
      bool finFetch = false) override;

  virtual compat::Expected<compat::Unit, MoQPublishError> endOfTrackAndGroup(
      uint64_t groupID,
      uint64_t subgroupID,
      uint64_t objectID) override;

  virtual compat::Expected<compat::Unit, MoQPublishError> endOfFetch() override;

  virtual void reset(ResetStreamErrorCode error) override;

  virtual folly::Expected<folly::SemiFuture<uint64_t>, MoQPublishError>
  awaitReadyToConsume() override;

 private:
  uint64_t fetchDataSent_{0};
};

class MoQPerfClient : public moxygen::Subscriber,
                      public std::enable_shared_from_this<MoQPerfClient> {
 public:
  MoQPerfClient(
      const folly::SocketAddress& peerAddr,
      folly::EventBase* evb,
      std::chrono::milliseconds connectTimeout,
      std::chrono::milliseconds transactionTimeout);

  folly::coro::Task<void> connect();

  folly::coro::Task<MoQSession::SubscribeResult> subscribe(
      std::shared_ptr<MoQPerfClientTrackConsumer> trackConsumer,
      MoQPerfParams params);

  folly::coro::Task<MoQSession::FetchResult> fetch(
      std::shared_ptr<MoQPerfClientFetchConsumer> fetchConsumer,
      MoQPerfParams params);

  void drain();

 private:
  std::unique_ptr<moxygen::MoQFollyExecutorImpl> moqExecutor_;
  moxygen::MoQClient moqClient_;
  std::chrono::milliseconds connectTimeout_;
  std::chrono::milliseconds transactionTimeout_;
};

} // namespace moxygen
