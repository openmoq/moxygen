/*
 *  Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 *  This source code is licensed under the MIT license found in the LICENSE
 *  file in the root directory of this source tree.
 *
 */

#pragma once

#include <moxygen/MoQTypes.h>
#include <moxygen/compat/Async.h>
#include <moxygen/compat/Callbacks.h>
#include <moxygen/compat/Expected.h>

#if MOXYGEN_USE_FOLLY
#include <folly/coro/Task.h>
#endif

// MoQ Subscriber interface
//
// This class is symmetric for the caller and callee.  MoQSession will implement
// Subscriber, and an application will optionally set Subbscriber callback on
// session.
//
// The caller will invoke:
//
//   auto publishNamespaceResult = co_await session->publishNamespace(...);
//
//   publishNamespaceResult.value() can be used for unnanounce
//
// And the remote peer will receive a callback
//
// folly::coro::Task<PublishNamespaceResult> publishNamespace(...) {
//   verify publishNamespace
//   create PublishNamespaceHandle
//   Fill in publishNamespaceOk
//   Current session can be obtained from folly::RequestContext
//   co_return handle;
// }

namespace moxygen {

class TrackConsumer;
class SubscriptionHandle;

// Represents a subscriber on which the caller can invoke PUBLISH_NAMESPACE
class Subscriber {
 public:
  using SubscriptionHandle = moxygen::SubscriptionHandle;
  virtual ~Subscriber() = default;

  // On successful PUBLISH_NAMESPACE, an PublishNamespaceHandle is returned,
  // which the caller can use to later PUBLISH_NAMESPACE_DONE.
  class PublishNamespaceHandle {
   public:
    PublishNamespaceHandle() = default;
    explicit PublishNamespaceHandle(PublishNamespaceOk annOk)
        : publishNamespaceOk_(std::move(annOk)) {}
    virtual ~PublishNamespaceHandle() = default;
    // Providing a default implementation of publishNamespaceDone, because it
    // can be an uninteresting message
    virtual void publishNamespaceDone() {}

    const PublishNamespaceOk& publishNamespaceOk() const {
      return *publishNamespaceOk_;
    }

   protected:
    void setPublishNamespaceOk(PublishNamespaceOk ok) {
      publishNamespaceOk_ = std::move(ok);
    };

    std::optional<PublishNamespaceOk> publishNamespaceOk_;
  };

  // A subscribe receives an PublishNamespaceCallback in publishNamespace, which
  // can be used to issue PUBLISH_NAMESPACE_CANCEL at some point after
  // PUBLISH_NAMESPACE_Ok.
  class PublishNamespaceCallback {
   public:
    virtual ~PublishNamespaceCallback() = default;

    virtual void publishNamespaceCancel(
        PublishNamespaceErrorCode errorCode,
        std::string reasonPhrase) = 0;
  };

  // Send/respond to PUBLISH_NAMESPACE
  using PublishNamespaceResult = compat::
      Expected<std::shared_ptr<PublishNamespaceHandle>, PublishNamespaceError>;
#if MOXYGEN_USE_FOLLY && MOXYGEN_QUIC_MVFST
  virtual compat::Task<PublishNamespaceResult> publishNamespace(
      PublishNamespace ann,
      std::shared_ptr<PublishNamespaceCallback> = nullptr) {
    return folly::coro::makeTask<PublishNamespaceResult>(compat::makeUnexpected(
        PublishNamespaceError{
            ann.requestID,
            PublishNamespaceErrorCode::NOT_SUPPORTED,
            "unimplemented"}));
  }
#else
  // Callback-based API for std-mode and Folly + picoquic
  virtual void publishNamespaceWithCallback(
      PublishNamespace ann,
      std::shared_ptr<PublishNamespaceCallback> cancelCallback,
      std::shared_ptr<compat::ResultCallback<
          std::shared_ptr<PublishNamespaceHandle>,
          PublishNamespaceError>> callback) {
    callback->onError(PublishNamespaceError{
        ann.requestID,
        PublishNamespaceErrorCode::NOT_SUPPORTED,
        "unimplemented"});
  }
#endif

  // Result of a PUBLISH request containing consumer and async reply
#if MOXYGEN_USE_FOLLY && MOXYGEN_QUIC_MVFST
  struct PublishConsumerAndReplyTask {
    std::shared_ptr<TrackConsumer> consumer;
    compat::Task<compat::Expected<PublishOk, PublishError>> reply;
  };
#else
  // Callback-based version for std-mode and Folly + picoquic
  struct PublishConsumerAndReplyCallback {
    std::shared_ptr<TrackConsumer> consumer;
    // In std-mode, the caller provides a callback that will be invoked
    // when the PublishOk/PublishError is received
    std::shared_ptr<compat::ResultCallback<PublishOk, PublishError>> callback;
  };
#endif

  // Send/respond to a PUBLISH - synchronous API o that the publisher can
  // immediately start sending data instead of waiting for a PUBLISH_OK from the
  // peer
#if MOXYGEN_USE_FOLLY && MOXYGEN_QUIC_MVFST
  using PublishResult =
      compat::Expected<PublishConsumerAndReplyTask, PublishError>;
  virtual PublishResult publish(
      PublishRequest pub,
      std::shared_ptr<SubscriptionHandle> /*handle*/ = nullptr) {
    return compat::makeUnexpected(
        PublishError{
            pub.requestID, PublishErrorCode::NOT_SUPPORTED, "unimplemented"});
  }
#else
  // Callback-based version for std-mode
  using PublishResult =
      compat::Expected<PublishConsumerAndReplyCallback, PublishError>;
  virtual PublishResult publish(
      PublishRequest pub,
      std::shared_ptr<SubscriptionHandle> /*handle*/ = nullptr) {
    return compat::makeUnexpected(
        PublishError{
            pub.requestID, PublishErrorCode::NOT_SUPPORTED, "unimplemented"});
  }
#endif

  virtual void goaway(Goaway /*goaway*/) {}
};

} // namespace moxygen
