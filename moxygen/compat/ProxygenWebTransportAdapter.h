/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <moxygen/compat/Config.h>

#if MOXYGEN_USE_FOLLY

#include <moxygen/compat/WebTransportInterface.h>

#include <folly/MaybeManagedPtr.h>
#include <proxygen/lib/http/webtransport/WebTransport.h>

namespace moxygen::compat {

// Adapter that wraps proxygen::WebTransport::StreamWriteHandle
class ProxygenStreamWriteHandle : public StreamWriteHandle {
 public:
  explicit ProxygenStreamWriteHandle(
      proxygen::WebTransport::StreamWriteHandle* handle)
      : handle_(handle) {}

  uint64_t getID() const override {
    return handle_->getID();
  }

  Expected<Unit, WebTransportError> writeStreamData(
      std::unique_ptr<Payload> data,
      bool fin) override {
    auto result = handle_->writeStreamData(data->releaseIOBuf(), fin);
    if (result.hasError()) {
      return makeUnexpected(
          static_cast<WebTransportError>(result.error()));
    }
    return unit;
  }

  void resetStream(uint32_t errorCode) override {
    handle_->resetStream(errorCode);
  }

  void setPeerCancelCallback(std::function<void(uint32_t)> cb) override {
    peerCancelCb_ = std::move(cb);
    // Register cancellation callback via CancellationToken
    if (peerCancelCb_) {
      cancelCallback_ = folly::CancellationCallback(
          handle_->getCancelToken(), [this]() {
            if (peerCancelCb_) {
              auto* ex = handle_->exception();
              peerCancelCb_(ex ? ex->error : 0);
            }
          });
    }
  }

  Expected<Unit, WebTransportError> registerDeliveryCallback(
      uint64_t offset,
      DeliveryCallback* cb) override {
    // Create adapter for the delivery callback
    if (cb) {
      deliveryAdapter_ = std::make_unique<DeliveryCallbackAdapter>(cb);
      auto result =
          handle_->registerDeliveryCallback(offset, deliveryAdapter_.get());
      if (result.hasError()) {
        return makeUnexpected(
            static_cast<WebTransportError>(result.error()));
      }
    }
    return unit;
  }

  // Get underlying handle for direct access when needed
  proxygen::WebTransport::StreamWriteHandle* getProxygenHandle() {
    return handle_;
  }

 private:
  // Adapter to convert our DeliveryCallback to proxygen's ByteEventCallback
  class DeliveryCallbackAdapter
      : public proxygen::WebTransport::ByteEventCallback {
   public:
    explicit DeliveryCallbackAdapter(DeliveryCallback* cb) : cb_(cb) {}

    void onByteEvent(quic::StreamId id, uint64_t offset) noexcept override {
      if (cb_) {
        cb_->onDelivery(id, offset);
      }
    }

    void onByteEventCanceled(
        quic::StreamId id,
        uint64_t offset) noexcept override {
      if (cb_) {
        cb_->onCancellation(id, offset);
      }
    }

   private:
    DeliveryCallback* cb_;
  };

  proxygen::WebTransport::StreamWriteHandle* handle_;
  std::function<void(uint32_t)> peerCancelCb_;
  std::optional<folly::CancellationCallback> cancelCallback_;
  std::unique_ptr<DeliveryCallbackAdapter> deliveryAdapter_;
};

// Adapter that wraps proxygen::WebTransport::StreamReadHandle
class ProxygenStreamReadHandle : public StreamReadHandle {
 public:
  explicit ProxygenStreamReadHandle(
      proxygen::WebTransport::StreamReadHandle* handle)
      : handle_(handle) {}

  uint64_t getID() const override {
    return handle_->getID();
  }

  void setReadCallback(
      std::function<void(StreamData, std::optional<uint32_t>)> cb) override {
    readCb_ = std::move(cb);
    if (readCb_) {
      scheduleRead();
    }
  }

  Expected<Unit, WebTransportError> stopSending(uint32_t error) override {
    auto result = handle_->stopSending(error);
    if (result.hasError()) {
      return makeUnexpected(
          static_cast<WebTransportError>(result.error()));
    }
    return unit;
  }

  // Get underlying handle for direct access when needed
  proxygen::WebTransport::StreamReadHandle* getProxygenHandle() {
    return handle_;
  }

 private:
  void scheduleRead() {
    handle_->awaitNextRead(
        nullptr, // Use inline executor
        [this](
            proxygen::WebTransport::StreamReadHandle*,
            uint64_t,
            folly::Try<proxygen::WebTransport::StreamData> data) {
          if (readCb_) {
            if (data.hasException()) {
              auto* ex =
                  data.tryGetExceptionObject<proxygen::WebTransport::Exception>();
              StreamData sd;
              readCb_(std::move(sd), ex ? ex->error : 0);
            } else {
              StreamData sd;
              if (data->data) {
                sd.data = std::make_unique<Payload>(std::move(data->data));
              }
              sd.fin = data->fin;
              readCb_(std::move(sd), std::nullopt);

              // Continue reading if not fin
              if (!data->fin) {
                scheduleRead();
              }
            }
          }
        });
  }

  proxygen::WebTransport::StreamReadHandle* handle_;
  std::function<void(StreamData, std::optional<uint32_t>)> readCb_;
};

// Adapter that wraps proxygen::WebTransport
class ProxygenWebTransportAdapter : public WebTransportInterface {
 public:
  explicit ProxygenWebTransportAdapter(
      folly::MaybeManagedPtr<proxygen::WebTransport> wt)
      : wt_(std::move(wt)) {}

  Expected<StreamWriteHandle*, WebTransportError> createUniStream() override {
    auto result = wt_->createUniStream();
    if (result.hasError()) {
      return makeUnexpected(
          static_cast<WebTransportError>(result.error()));
    }
    auto adapter = std::make_unique<ProxygenStreamWriteHandle>(*result);
    auto* ptr = adapter.get();
    writeHandles_.push_back(std::move(adapter));
    return ptr;
  }

  Expected<BidiStreamHandle*, WebTransportError> createBidiStream() override {
    auto result = wt_->createBidiStream();
    if (result.hasError()) {
      return makeUnexpected(
          static_cast<WebTransportError>(result.error()));
    }
    // TODO: Implement BidiStreamHandle adapter
    return makeUnexpected(WebTransportError::GENERIC_ERROR);
  }

  Expected<Unit, WebTransportError> sendDatagram(
      std::unique_ptr<Payload> data) override {
    auto result = wt_->sendDatagram(data->releaseIOBuf());
    if (result.hasError()) {
      return makeUnexpected(
          static_cast<WebTransportError>(result.error()));
    }
    return unit;
  }

  void closeSession(uint32_t errorCode) override {
    wt_->closeSession(errorCode);
  }

  SocketAddress getPeerAddress() const override {
    auto addr = wt_->getPeerAddress();
    return SocketAddress(addr.getAddressStr(), addr.getPort());
  }

  void setNewUniStreamCallback(
      std::function<void(StreamReadHandle*)> cb) override {
    newUniStreamCb_ = std::move(cb);
  }

  void setNewBidiStreamCallback(
      std::function<void(BidiStreamHandle*)> cb) override {
    newBidiStreamCb_ = std::move(cb);
  }

  void setDatagramCallback(
      std::function<void(std::unique_ptr<Payload>)> cb) override {
    datagramCb_ = std::move(cb);
  }

  void setSessionCloseCallback(
      std::function<void(std::optional<uint32_t>)> cb) override {
    sessionCloseCb_ = std::move(cb);
  }

  // Get underlying WebTransport for direct access when needed
  proxygen::WebTransport* getProxygenWebTransport() {
    return wt_.get();
  }

 private:
  folly::MaybeManagedPtr<proxygen::WebTransport> wt_;
  std::vector<std::unique_ptr<ProxygenStreamWriteHandle>> writeHandles_;
  std::vector<std::unique_ptr<ProxygenStreamReadHandle>> readHandles_;
  std::function<void(StreamReadHandle*)> newUniStreamCb_;
  std::function<void(BidiStreamHandle*)> newBidiStreamCb_;
  std::function<void(std::unique_ptr<Payload>)> datagramCb_;
  std::function<void(std::optional<uint32_t>)> sessionCloseCb_;
};

} // namespace moxygen::compat

#endif // MOXYGEN_USE_FOLLY
