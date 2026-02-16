/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <moxygen/transports/PicoquicMoQRelayClient.h>

#if MOXYGEN_QUIC_PICOQUIC

#include <moxygen/MoQRelaySession.h>
#include <moxygen/compat/Debug.h>

namespace moxygen::transports {

PicoquicMoQRelayClient::PicoquicMoQRelayClient(
    std::shared_ptr<MoQExecutor> executor,
    PicoquicMoQClient::Config config)
    : executor_(std::move(executor)) {
  // Use relay session factory to create MoQRelaySession instead of MoQSession
  config.sessionFactory = MoQRelaySession::createRelaySessionFactory();

  // Create the underlying client
  client_ = std::make_unique<PicoquicMoQClient>(executor_, std::move(config));
}

PicoquicMoQRelayClient::~PicoquicMoQRelayClient() {
  shutdown();
}

void PicoquicMoQRelayClient::setupWithCallback(
    std::shared_ptr<Publisher> publisher,
    std::shared_ptr<Subscriber> subscriber,
    std::chrono::milliseconds connectTimeout,
    std::shared_ptr<
        compat::ResultCallback<compat::Unit, SessionCloseErrorCode>> callback) {
  XLOG(INFO) << "PicoquicMoQRelayClient::setupWithCallback";

  auto connectCallback = compat::makeResultCallback<
      compat::MoQClientInterface::ConnectResult,
      compat::MoQClientInterface::ConnectError>(
      [callback](compat::MoQClientInterface::ConnectResult /* result */) {
        XLOG(INFO) << "PicoquicMoQRelayClient connected successfully";
        callback->onSuccess();
      },
      [callback](compat::MoQClientInterface::ConnectError error) {
        XLOG(ERR) << "PicoquicMoQRelayClient connection error: "
                  << static_cast<uint32_t>(error);
        callback->onError(error);
      });

  client_->connectWithCallback(
      connectTimeout,
      std::move(publisher),
      std::move(subscriber),
      std::move(connectCallback));
}

void PicoquicMoQRelayClient::run(
    std::shared_ptr<Publisher> /* publisher */,
    std::vector<TrackNamespace> namespaces,
    std::shared_ptr<compat::ResultCallback<compat::Unit, PublishNamespaceError>>
        callback) {
  XLOG(INFO) << "PicoquicMoQRelayClient::run with " << namespaces.size()
             << " namespaces";

  auto session = client_->getSession();
  if (!session) {
    XLOG(ERR) << "Session is null in run()";
    callback->onError(PublishNamespaceError{
        RequestID(0),
        PublishNamespaceErrorCode::INTERNAL_ERROR,
        "Session is null"});
    return;
  }

  // Cast to MoQRelaySession to access publishNamespaceWithCallback
  auto relaySession = std::dynamic_pointer_cast<MoQRelaySession>(session);
  if (!relaySession) {
    XLOG(ERR) << "Session is not a MoQRelaySession";
    callback->onError(PublishNamespaceError{
        RequestID(0),
        PublishNamespaceErrorCode::INTERNAL_ERROR,
        "Session is not a relay session"});
    return;
  }

  if (namespaces.empty()) {
    callback->onSuccess();
    return;
  }

  // Track progress for all namespace announcements
  auto remaining =
      std::make_shared<std::atomic<size_t>>(namespaces.size());
  auto hasError = std::make_shared<std::atomic<bool>>(false);
  auto weakThis = std::weak_ptr<PicoquicMoQRelayClient>();

  // Capture this for storing handles
  auto* self = this;

  for (auto& ns : namespaces) {
    PublishNamespace ann;
    ann.trackNamespace = std::move(ns);

    auto announceCallback = compat::makeResultCallback<
        std::shared_ptr<Subscriber::PublishNamespaceHandle>,
        PublishNamespaceError>(
        [self, remaining, hasError, callback](
            std::shared_ptr<Subscriber::PublishNamespaceHandle> handle) {
          XLOG(INFO) << "PublishNamespace succeeded";
          self->publishNamespaceHandles_.emplace_back(std::move(handle));

          size_t left = remaining->fetch_sub(1) - 1;
          if (left == 0 && !hasError->load()) {
            callback->onSuccess();
          }
        },
        [remaining, hasError, callback](PublishNamespaceError error) {
          XLOG(ERR) << "PublishNamespace error: " << error.reasonPhrase;
          bool expected = false;
          if (hasError->compare_exchange_strong(expected, true)) {
            // First error - report it
            callback->onError(std::move(error));
          }
          remaining->fetch_sub(1);
        });

    relaySession->publishNamespaceWithCallback(
        std::move(ann),
        nullptr, // cancelCallback
        std::move(announceCallback));
  }
}

std::shared_ptr<MoQSession> PicoquicMoQRelayClient::getSession() const {
  return client_ ? client_->getSession() : nullptr;
}

void PicoquicMoQRelayClient::shutdown() {
  XLOG(INFO) << "PicoquicMoQRelayClient::shutdown";

  // Release all publish namespace handles
  for (auto& handle : publishNamespaceHandles_) {
    if (handle) {
      handle->publishNamespaceDone();
    }
  }
  publishNamespaceHandles_.clear();

  // Close the session
  if (client_) {
    auto session = client_->getSession();
    if (session) {
      session->setPublishHandler(nullptr);
      session->setSubscribeHandler(nullptr);
      session->close(SessionCloseErrorCode::NO_ERROR);
    }
  }
}

} // namespace moxygen::transports

#endif // MOXYGEN_QUIC_PICOQUIC
