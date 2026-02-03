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
  beginSubgroup(uint64_t groupID, uint64_t subgroupID, Priority priority)
      override {
    return downstream_->beginSubgroup(groupID, subgroupID, priority);
  }

  compat::Expected<compat::SemiFuture<compat::Unit>, MoQPublishError>
  awaitStreamCredit() override {
    return downstream_->awaitStreamCredit();
  }

  compat::Expected<compat::Unit, MoQPublishError> objectStream(
      const ObjectHeader& header,
      Payload payload) override {
    return downstream_->objectStream(header, std::move(payload));
  }

  compat::Expected<compat::Unit, MoQPublishError> datagram(
      const ObjectHeader& header,
      Payload payload) override {
    return downstream_->datagram(header, std::move(payload));
  }

  compat::Expected<compat::Unit, MoQPublishError> subscribeDone(
      SubscribeDone subDone) override {
    return downstream_->subscribeDone(std::move(subDone));
  }

  void setDeliveryCallback(
      std::shared_ptr<DeliveryCallback> callback) override {
    downstream_->setDeliveryCallback(std::move(callback));
  }

 private:
  std::shared_ptr<TrackConsumer> downstream_;
};

} // namespace moxygen
