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

// MoQ Publisher interface
//
// This class is symmetric for the caller and callee.  MoQSession will implement
// Publisher, and an application will optionally set a Publisher callback.
//
// The caller will invoke:
//
//   auto subscribeResult = co_await session->subscribe(...);
//
//   subscribeResult.value() can be used for subscribeUpdate and unsubscribe
//
// And the remote peer will receive a callback
//
// compat::Task<SubscribeResult> subscribe(...) {
//   verify subscribe
//   create SubscriptionHandle
//   Fill in subscribeOK
//   Current session can be obtained from folly::RequestContext
//   co_return handle;
// }

namespace moxygen {

class FetchConsumer;
class TrackConsumer;

class SubscriptionHandle {
 public:
  SubscriptionHandle() = default;
  explicit SubscriptionHandle(SubscribeOk ok) : subscribeOk_(std::move(ok)) {}
  virtual ~SubscriptionHandle() = default;

  virtual void unsubscribe() = 0;

  using RequestUpdateResult = compat::Expected<RequestOk, RequestError>;
  // Updates request parameters (start/end locations, priority, forward).
  // This is a coroutine because it may do async work, such as forwarding the
  // update to the upstream publisher in a relay scenario.
#if MOXYGEN_USE_FOLLY && MOXYGEN_QUIC_MVFST
  virtual compat::Task<RequestUpdateResult> requestUpdate(
      RequestUpdate reqUpdate) = 0;
#else
  virtual void requestUpdateWithCallback(
      RequestUpdate reqUpdate,
      std::shared_ptr<compat::ResultCallback<RequestOk, RequestError>>
          callback) {
    callback->onError(RequestError{
        reqUpdate.requestID,
        RequestErrorCode::NOT_SUPPORTED,
        "Request update not implemented"});
  }
#endif

  const SubscribeOk& subscribeOk() const {
    return *subscribeOk_;
  }

 protected:
  void setSubscribeOk(SubscribeOk subOk) {
    subscribeOk_ = std::move(subOk);
  }
  std::optional<SubscribeOk> subscribeOk_;
};

// Represents a publisher on which the caller can invoke TRACK_STATUS_REQUEST,
// SUBSCRIBE, FETCH and SUBSCRIBE_NAMESPACE.
class Publisher {
 public:
  using SubscriptionHandle = moxygen::SubscriptionHandle;
  virtual ~Publisher() = default;

  // Send/respond to TRACK_STATUS_REQUEST
  using TrackStatusResult = compat::Expected<TrackStatusOk, TrackStatusError>;
#if MOXYGEN_USE_FOLLY && MOXYGEN_QUIC_MVFST
  virtual compat::Task<TrackStatusResult> trackStatus(
      const TrackStatus trackStatus) {
    return folly::coro::makeTask<TrackStatusResult>(
        compat::makeUnexpected<TrackStatusError>(TrackStatusError{
            trackStatus.requestID,
            TrackStatusErrorCode::NOT_SUPPORTED,
            "Track status not implemented"}));
  }
#else
  // Callback-based API for std-mode and Folly + picoquic
  virtual void trackStatusWithCallback(
      const TrackStatus trackStatus,
      std::shared_ptr<compat::ResultCallback<TrackStatusOk, TrackStatusError>>
          callback) {
    callback->onError(TrackStatusError{
        trackStatus.requestID,
        TrackStatusErrorCode::NOT_SUPPORTED,
        "Track status not implemented"});
  }
#endif

  // On successful SUBSCRIBE, a SubscriptionHandle is returned, which the
  // caller can use to UNSUBSCRIBE or SUBSCRIBE_UPDATE.
  // Send/respond to a SUBSCRIBE
  using SubscribeResult =
      compat::Expected<std::shared_ptr<SubscriptionHandle>, SubscribeError>;
#if MOXYGEN_USE_FOLLY && MOXYGEN_QUIC_MVFST
  virtual compat::Task<SubscribeResult> subscribe(
      SubscribeRequest sub,
      std::shared_ptr<TrackConsumer> callback) {
    return folly::coro::makeTask<SubscribeResult>(compat::makeUnexpected(
        SubscribeError{
            sub.requestID,
            SubscribeErrorCode::NOT_SUPPORTED,
            "unimplemented"}));
  }
#else
  // Callback-based API for std-mode
  virtual void subscribeWithCallback(
      SubscribeRequest sub,
      std::shared_ptr<TrackConsumer> consumer,
      std::shared_ptr<compat::ResultCallback<
          std::shared_ptr<SubscriptionHandle>,
          SubscribeError>> callback) {
    callback->onError(SubscribeError{
        sub.requestID, SubscribeErrorCode::NOT_SUPPORTED, "unimplemented"});
  }
#endif

  // On successful FETCH, a FetchHandle is returned, which the caller can use
  // to FETCH_CANCEL.
  class FetchHandle {
   public:
    FetchHandle() = default;
    explicit FetchHandle(FetchOk ok) : fetchOk_(std::move(ok)) {}
    virtual ~FetchHandle() = default;

    virtual void fetchCancel() = 0;

    using RequestUpdateResult = compat::Expected<RequestOk, RequestError>;
#if MOXYGEN_USE_FOLLY && MOXYGEN_QUIC_MVFST
    virtual compat::Task<RequestUpdateResult> requestUpdate(
        RequestUpdate reqUpdate) = 0;
#else
    virtual void requestUpdateWithCallback(
        RequestUpdate reqUpdate,
        std::shared_ptr<compat::ResultCallback<RequestOk, RequestError>>
            callback) {
      callback->onError(RequestError{
          reqUpdate.requestID,
          RequestErrorCode::NOT_SUPPORTED,
          "Request update not implemented"});
    }
#endif

    const FetchOk& fetchOk() const {
      return *fetchOk_;
    }

   protected:
    void setFetchOk(FetchOk fOk) {
      fetchOk_ = std::move(fOk);
    }

    std::optional<FetchOk> fetchOk_;
  };

  // Send/respond to a FETCH
  using FetchResult = compat::Expected<std::shared_ptr<FetchHandle>, FetchError>;
#if MOXYGEN_USE_FOLLY && MOXYGEN_QUIC_MVFST
  virtual compat::Task<FetchResult> fetch(
      Fetch fetch,
      std::shared_ptr<FetchConsumer> fetchCallback) {
    return folly::coro::makeTask<FetchResult>(compat::makeUnexpected(
        FetchError{
            fetch.requestID, FetchErrorCode::NOT_SUPPORTED, "unimplemented"}));
  }
#else
  // Callback-based API for std-mode and Folly + picoquic
  virtual void fetchWithCallback(
      Fetch fetch,
      std::shared_ptr<FetchConsumer> consumer,
      std::shared_ptr<
          compat::ResultCallback<std::shared_ptr<FetchHandle>, FetchError>>
          callback) {
    callback->onError(FetchError{
        fetch.requestID, FetchErrorCode::NOT_SUPPORTED, "unimplemented"});
  }
#endif

  // On successful SUBSCRIBE_NAMESPACE, a SubscribeNamespaceHandle is returned,
  // which the caller can use to UNSUBSCRIBE_NAMESPACE
  class SubscribeNamespaceHandle {
   public:
    SubscribeNamespaceHandle() = default;
    explicit SubscribeNamespaceHandle(SubscribeNamespaceOk ok)
        : subscribeNamespaceOk_(std::move(ok)) {}
    virtual ~SubscribeNamespaceHandle() = default;

    virtual void unsubscribeNamespace() = 0;

    using RequestUpdateResult = compat::Expected<RequestOk, RequestError>;
#if MOXYGEN_USE_FOLLY && MOXYGEN_QUIC_MVFST
    virtual compat::Task<RequestUpdateResult> requestUpdate(
        RequestUpdate reqUpdate) = 0;
#else
    virtual void requestUpdateWithCallback(
        RequestUpdate reqUpdate,
        std::shared_ptr<compat::ResultCallback<RequestOk, RequestError>>
            callback) {
      callback->onError(RequestError{
          reqUpdate.requestID,
          RequestErrorCode::NOT_SUPPORTED,
          "Request update not implemented"});
    }
#endif

    const SubscribeNamespaceOk& subscribeNamespaceOk() const {
      return *subscribeNamespaceOk_;
    }

   protected:
    void setSubscribeNamespaceOk(SubscribeNamespaceOk ok) {
      subscribeNamespaceOk_ = std::move(ok);
    }

    std::optional<SubscribeNamespaceOk> subscribeNamespaceOk_;
  };

  class NamespacePublishHandle {
   public:
    virtual ~NamespacePublishHandle() = default;

    virtual void namespaceMsg(const TrackNamespace& trackNamespaceSuffix) = 0;

    virtual void namespaceDoneMsg() = 0;
  };

  // Send/respond to SUBSCRIBE_NAMESPACE
  using SubscribeNamespaceResult = compat::Expected<
      std::shared_ptr<SubscribeNamespaceHandle>,
      SubscribeNamespaceError>;
#if MOXYGEN_USE_FOLLY && MOXYGEN_QUIC_MVFST
  virtual compat::Task<SubscribeNamespaceResult> subscribeNamespace(
      SubscribeNamespace subAnn,
      std::shared_ptr<NamespacePublishHandle>
          namespacePublishHandle /* draft 16+ */) {
    return folly::coro::makeTask<SubscribeNamespaceResult>(
        compat::makeUnexpected(
            SubscribeNamespaceError{
                subAnn.requestID,
                SubscribeNamespaceErrorCode::NOT_SUPPORTED,
                "unimplemented"}));
  }
#else
  // Callback-based API for std-mode and Folly + picoquic
  virtual void subscribeNamespaceWithCallback(
      SubscribeNamespace subAnn,
      std::shared_ptr<compat::ResultCallback<
          std::shared_ptr<SubscribeNamespaceHandle>,
          SubscribeNamespaceError>> callback) {
    callback->onError(SubscribeNamespaceError{
        subAnn.requestID,
        SubscribeNamespaceErrorCode::NOT_SUPPORTED,
        "unimplemented"});
  }
#endif

  virtual void goaway(Goaway /*goaway*/) {}
};

} // namespace moxygen
