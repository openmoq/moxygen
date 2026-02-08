#pragma once

#include <moxygen/MoQConsumers.h>

namespace moxygen {
class TrackConsumerFilter : public TrackConsumer {
 public:
  explicit TrackConsumerFilter(std::shared_ptr<TrackConsumer> downstream)
      : downstream_(std::move(downstream)) {}

  compat::Expected<compat::Unit, MoQPublishError> setTrackAlias(
      TrackAlias alias) override {
    return downstream_->setTrackAlias(alias);
  }

  compat::Expected<std::shared_ptr<SubgroupConsumer>, MoQPublishError>
  beginSubgroup(
      uint64_t groupID,
      uint64_t subgroupID,
      Priority priority,
      bool containsLastInGroup = false) override {
    return downstream_->beginSubgroup(
        groupID, subgroupID, priority, containsLastInGroup);
  }

  compat::Expected<compat::SemiFuture<compat::Unit>, MoQPublishError>
  awaitStreamCredit() override {
    return downstream_->awaitStreamCredit();
  }

  compat::Expected<compat::Unit, MoQPublishError> objectStream(
      const ObjectHeader& header,
      Payload payload,
      bool lastInGroup = false) override {
    return downstream_->objectStream(header, std::move(payload), lastInGroup);
  }

  compat::Expected<compat::Unit, MoQPublishError> datagram(
      const ObjectHeader& header,
      Payload payload,
      bool lastInGroup = false) override {
    return downstream_->datagram(header, std::move(payload), lastInGroup);
  }

  compat::Expected<compat::Unit, MoQPublishError> publishDone(
      PublishDone pubDone) override {
    return downstream_->publishDone(std::move(pubDone));
  }

  void setDeliveryCallback(
      std::shared_ptr<DeliveryCallback> callback) override {
    downstream_->setDeliveryCallback(std::move(callback));
  }

 private:
  std::shared_ptr<TrackConsumer> downstream_;
};

} // namespace moxygen
