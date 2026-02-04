/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "moxygen/transports/ProxygenWebTransportAdapter.h"

#if MOXYGEN_QUIC_MVFST

#include <folly/io/IOBuf.h>
#include <quic/priority/HTTPPriorityQueue.h>

namespace moxygen::transports {

// --- ProxygenWebTransportAdapter Implementation ---

ProxygenWebTransportAdapter::ProxygenWebTransportAdapter(
    folly::MaybeManagedPtr<proxygen::WebTransport> wt)
    : wt_(std::move(wt)) {}

ProxygenWebTransportAdapter::~ProxygenWebTransportAdapter() {
  writeHandles_.clear();
  readHandles_.clear();
  bidiHandles_.clear();
}

compat::Expected<compat::StreamWriteHandle*, compat::WebTransportError>
ProxygenWebTransportAdapter::createUniStream() {
  auto result = wt_->createUniStream();
  if (!result) {
    return compat::makeUnexpected(compat::WebTransportError::STREAM_CREATION_ERROR);
  }

  auto* proxygenHandle = *result;
  auto streamId = proxygenHandle->getID();

  auto wrapper = std::make_unique<ProxygenStreamWriteHandle>(proxygenHandle);
  auto* ptr = wrapper.get();
  writeHandles_[streamId] = std::move(wrapper);

  return ptr;
}

compat::Expected<compat::BidiStreamHandle*, compat::WebTransportError>
ProxygenWebTransportAdapter::createBidiStream() {
  auto result = wt_->createBidiStream();
  if (!result) {
    return compat::makeUnexpected(compat::WebTransportError::STREAM_CREATION_ERROR);
  }

  auto& bh = *result;
  auto streamId = bh.readHandle->getID();

  auto writeWrapper =
      std::make_unique<ProxygenStreamWriteHandle>(bh.writeHandle);
  auto readWrapper = std::make_unique<ProxygenStreamReadHandle>(bh.readHandle);

  auto bidiWrapper = std::make_unique<ProxygenBidiStreamHandle>(
      std::move(writeWrapper), std::move(readWrapper));
  auto* ptr = bidiWrapper.get();
  bidiHandles_[streamId] = std::move(bidiWrapper);

  return ptr;
}

compat::Expected<compat::Unit, compat::WebTransportError>
ProxygenWebTransportAdapter::sendDatagram(
    std::unique_ptr<compat::Payload> data) {
  if (!data) {
    return compat::makeUnexpected(compat::WebTransportError::SEND_ERROR);
  }

  // Convert Payload to IOBuf
  auto iobuf = folly::IOBuf::copyBuffer(data->data(), data->length());

  auto result = wt_->sendDatagram(std::move(iobuf));
  if (!result) {
    return compat::makeUnexpected(compat::WebTransportError::SEND_ERROR);
  }

  return compat::Unit{};
}

void ProxygenWebTransportAdapter::setMaxDatagramSize(size_t /*maxSize*/) {
  // proxygen doesn't have a direct API for this
}

size_t ProxygenWebTransportAdapter::getMaxDatagramSize() const {
  // Return a reasonable default
  return 1200;
}

void ProxygenWebTransportAdapter::closeSession(uint32_t errorCode) {
  wt_->closeSession(errorCode);
}

void ProxygenWebTransportAdapter::drainSession() {
  // proxygen WebTransport doesn't have a direct drain API
  // We can close with no error
  wt_->closeSession(0);
}

compat::SocketAddress ProxygenWebTransportAdapter::getPeerAddress() const {
  return wt_->getPeerAddress();
}

compat::SocketAddress ProxygenWebTransportAdapter::getLocalAddress() const {
  return wt_->getLocalAddress();
}

std::string ProxygenWebTransportAdapter::getALPN() const {
  // proxygen doesn't expose ALPN directly through WebTransport
  // Return empty for now - caller can use other means to get it
  return "";
}

bool ProxygenWebTransportAdapter::isConnected() const {
  // proxygen doesn't expose this directly
  // Assume connected if we have a valid transport
  return wt_.get() != nullptr;
}

void ProxygenWebTransportAdapter::setNewUniStreamCallback(
    std::function<void(compat::StreamReadHandle*)> cb) {
  newUniStreamCb_ = std::move(cb);
}

void ProxygenWebTransportAdapter::setNewBidiStreamCallback(
    std::function<void(compat::BidiStreamHandle*)> cb) {
  newBidiStreamCb_ = std::move(cb);
}

void ProxygenWebTransportAdapter::setDatagramCallback(
    std::function<void(std::unique_ptr<compat::Payload>)> cb) {
  datagramCb_ = std::move(cb);
}

void ProxygenWebTransportAdapter::setSessionCloseCallback(
    std::function<void(std::optional<uint32_t> error)> cb) {
  sessionCloseCb_ = std::move(cb);
}

void ProxygenWebTransportAdapter::setSessionDrainCallback(
    std::function<void()> cb) {
  sessionDrainCb_ = std::move(cb);
}

// --- ProxygenStreamWriteHandle Implementation ---

ProxygenStreamWriteHandle::ProxygenStreamWriteHandle(
    proxygen::WebTransport::StreamWriteHandle* handle)
    : handle_(handle) {}

uint64_t ProxygenStreamWriteHandle::getID() const {
  return handle_->getID();
}

void ProxygenStreamWriteHandle::writeStreamData(
    std::unique_ptr<compat::Payload> data,
    bool fin,
    std::function<void(bool success)> callback) {
  // Convert Payload to IOBuf
  std::unique_ptr<folly::IOBuf> iobuf;
  if (data && data->length() > 0) {
    iobuf = folly::IOBuf::copyBuffer(data->data(), data->length());
  }

  auto result = handle_->writeStreamData(std::move(iobuf), fin, nullptr);
  if (callback) {
    callback(result.hasValue());
  }
}

compat::Expected<compat::Unit, compat::WebTransportError>
ProxygenStreamWriteHandle::writeStreamDataSync(
    std::unique_ptr<compat::Payload> data,
    bool fin) {
  std::unique_ptr<folly::IOBuf> iobuf;
  if (data && data->length() > 0) {
    iobuf = folly::IOBuf::copyBuffer(data->data(), data->length());
  }

  auto result = handle_->writeStreamData(std::move(iobuf), fin, nullptr);
  if (!result) {
    return compat::makeUnexpected(compat::WebTransportError::SEND_ERROR);
  }

  return compat::Unit{};
}

void ProxygenStreamWriteHandle::resetStream(uint32_t errorCode) {
  handle_->resetStream(errorCode);
}

void ProxygenStreamWriteHandle::setPriority(
    const compat::StreamPriority& priority) {
  handle_->setPriority(
      quic::HTTPPriorityQueue::Priority(
          priority.urgency, priority.incremental, priority.order));
}

void ProxygenStreamWriteHandle::awaitWritable(std::function<void()> callback) {
  // proxygen's awaitWritable returns a SemiFuture
  // For now, just invoke callback immediately (assuming writable)
  // Full implementation would need to handle the future
  if (callback) {
    callback();
  }
}

void ProxygenStreamWriteHandle::setPeerCancelCallback(
    std::function<void(uint32_t)> cb) {
  cancelCb_ = std::move(cb);
  // Note: proxygen uses getCancelToken() for this, which is more complex
  // A full implementation would need to integrate with folly::CancellationToken
}

compat::Expected<compat::Unit, compat::WebTransportError>
ProxygenStreamWriteHandle::registerDeliveryCallback(
    uint64_t offset,
    compat::DeliveryCallback* cb) {
  if (!cb) {
    return compat::Unit{};
  }

  // Create a wrapper callback that bridges to our interface
  class DeliveryCallbackWrapper
      : public proxygen::WebTransport::ByteEventCallback {
   public:
    DeliveryCallbackWrapper(
        uint64_t streamId,
        compat::DeliveryCallback* cb)
        : streamId_(streamId), cb_(cb) {}

    void onByteEvent(quic::StreamId /*streamId*/, uint64_t offset) noexcept override {
      if (cb_) {
        cb_->onDelivery(streamId_, offset);
      }
    }

    void onByteEventCanceled(quic::StreamId /*streamId*/, uint64_t offset) noexcept override {
      if (cb_) {
        cb_->onCancellation(streamId_, offset);
      }
    }

   private:
    uint64_t streamId_;
    compat::DeliveryCallback* cb_;
  };

  // Store the wrapper and use it with writeStreamData calls
  // Note: proxygen's registerDeliveryCallback is done via the ByteEventCallback
  // passed to writeStreamData, not as a separate call
  (void)offset;
  (void)cb;
  // For now, return success - proper implementation would need to pass the
  // callback to the next writeStreamData call
  return compat::Unit{};
}

bool ProxygenStreamWriteHandle::isCancelled() const {
  return cancelError_.has_value();
}

std::optional<uint32_t> ProxygenStreamWriteHandle::getCancelError() const {
  return cancelError_;
}

// --- ProxygenStreamReadHandle Implementation ---

ProxygenStreamReadHandle::ProxygenStreamReadHandle(
    proxygen::WebTransport::StreamReadHandle* handle)
    : handle_(handle) {}

uint64_t ProxygenStreamReadHandle::getID() const {
  return handle_->getID();
}

void ProxygenStreamReadHandle::setReadCallback(
    std::function<void(compat::StreamData, std::optional<uint32_t> error)>
        /*cb*/) {
  // proxygen uses a pull model (readStreamData returns a SemiFuture)
  // For callback-based reading, we'd need to set up a read loop
  // This is a placeholder - full implementation needs async integration
}

void ProxygenStreamReadHandle::pauseReading() {
  // proxygen's pull model doesn't have explicit pause/resume
}

void ProxygenStreamReadHandle::resumeReading() {
  // proxygen's pull model doesn't have explicit pause/resume
}

compat::Expected<compat::Unit, compat::WebTransportError>
ProxygenStreamReadHandle::stopSending(uint32_t error) {
  handle_->stopSending(error);
  return compat::Unit{};
}

bool ProxygenStreamReadHandle::isFinished() const {
  return finished_;
}

// --- ProxygenBidiStreamHandle Implementation ---

ProxygenBidiStreamHandle::ProxygenBidiStreamHandle(
    std::unique_ptr<ProxygenStreamWriteHandle> write,
    std::unique_ptr<ProxygenStreamReadHandle> read)
    : writeHandle_(std::move(write)), readHandle_(std::move(read)) {}

} // namespace moxygen::transports

#endif // MOXYGEN_QUIC_MVFST
