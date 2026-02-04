/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "moxygen/transports/PicoquicWebTransport.h"

#if MOXYGEN_QUIC_PICOQUIC

#include <moxygen/compat/Expected.h>
#include <moxygen/compat/Unit.h>
#include <cstring>

namespace moxygen::transports {

// --- PicoquicWebTransport Implementation ---

PicoquicWebTransport::PicoquicWebTransport(picoquic_cnx_t* cnx, bool isServer)
    : cnx_(cnx), isServer_(isServer) {
  // Set ourselves as the callback context
  picoquic_set_callback(cnx_, picoCallback, this);
}

PicoquicWebTransport::~PicoquicWebTransport() {
  if (!closed_) {
    closeSession(0);
  }
}

int PicoquicWebTransport::picoCallback(
    picoquic_cnx_t* cnx,
    uint64_t stream_id,
    uint8_t* bytes,
    size_t length,
    picoquic_call_back_event_t fin_or_event,
    void* callback_ctx,
    void* stream_ctx) {
  auto* self = static_cast<PicoquicWebTransport*>(callback_ctx);
  if (!self) {
    return 0;
  }

  return self->onStreamData(stream_id, bytes, length, fin_or_event, stream_ctx);
}

int PicoquicWebTransport::onStreamData(
    uint64_t streamId,
    uint8_t* bytes,
    size_t length,
    picoquic_call_back_event_t event,
    void* /*streamCtx*/) {
  switch (event) {
    case picoquic_callback_stream_data:
    case picoquic_callback_stream_fin: {
      bool fin = (event == picoquic_callback_stream_fin);

      // Check if this is an existing read stream
      auto readIt = readStreams_.find(streamId);
      if (readIt != readStreams_.end()) {
        readIt->second->onData(bytes, length, fin);
        return 0;
      }

      // Check bidirectional streams
      auto bidiIt = bidiStreams_.find(streamId);
      if (bidiIt != bidiStreams_.end()) {
        auto* readHandle =
            static_cast<PicoquicStreamReadHandle*>(bidiIt->second->readHandle());
        readHandle->onData(bytes, length, fin);
        return 0;
      }

      // New stream - determine type and create handle
      bool isClientInitiated = (streamId & 1) == 0;
      bool isBidi = (streamId & 2) == 0;

      if (isBidi) {
        // New bidirectional stream
        auto write = std::make_unique<PicoquicStreamWriteHandle>(cnx_, streamId);
        auto read = std::make_unique<PicoquicStreamReadHandle>(cnx_, streamId);
        auto* readPtr = read.get();

        auto bidi = std::make_unique<PicoquicBidiStreamHandle>(
            std::move(write), std::move(read));
        auto* bidiPtr = bidi.get();
        bidiStreams_[streamId] = std::move(bidi);

        readPtr->onData(bytes, length, fin);

        if (newBidiStreamCb_) {
          newBidiStreamCb_(bidiPtr);
        }
      } else {
        // New unidirectional stream
        auto read = std::make_unique<PicoquicStreamReadHandle>(cnx_, streamId);
        auto* readPtr = read.get();
        readStreams_[streamId] = std::move(read);

        readPtr->onData(bytes, length, fin);

        if (newUniStreamCb_) {
          newUniStreamCb_(readPtr);
        }
      }
      break;
    }

    case picoquic_callback_datagram:
      onDatagram(bytes, length);
      break;

    case picoquic_callback_stream_reset: {
      // Stream was reset by peer
      auto readIt = readStreams_.find(streamId);
      if (readIt != readStreams_.end()) {
        readIt->second->onError(0); // TODO: get actual error code
      }
      break;
    }

    case picoquic_callback_stop_sending: {
      // Peer sent STOP_SENDING
      auto writeIt = writeStreams_.find(streamId);
      if (writeIt != writeStreams_.end()) {
        // TODO: invoke cancel callback
      }
      break;
    }

    case picoquic_callback_close:
    case picoquic_callback_application_close:
      closed_ = true;
      if (sessionCloseCb_) {
        sessionCloseCb_(std::nullopt); // TODO: get error code
      }
      break;

    default:
      break;
  }

  return 0;
}

void PicoquicWebTransport::onDatagram(uint8_t* bytes, size_t length) {
  if (datagramCb_ && bytes && length > 0) {
    auto payload = std::make_unique<compat::Payload>(bytes, bytes + length);
    datagramCb_(std::move(payload));
  }
}

compat::Expected<compat::StreamWriteHandle*, compat::WebTransportError>
PicoquicWebTransport::createUniStream() {
  uint64_t streamId = picoquic_get_next_local_stream_id(cnx_, false);

  auto handle = std::make_unique<PicoquicStreamWriteHandle>(cnx_, streamId);
  auto* ptr = handle.get();
  writeStreams_[streamId] = std::move(handle);

  return ptr;
}

compat::Expected<compat::BidiStreamHandle*, compat::WebTransportError>
PicoquicWebTransport::createBidiStream() {
  uint64_t streamId = picoquic_get_next_local_stream_id(cnx_, true);

  auto write = std::make_unique<PicoquicStreamWriteHandle>(cnx_, streamId);
  auto read = std::make_unique<PicoquicStreamReadHandle>(cnx_, streamId);

  auto bidi = std::make_unique<PicoquicBidiStreamHandle>(
      std::move(write), std::move(read));
  auto* ptr = bidi.get();
  bidiStreams_[streamId] = std::move(bidi);

  return ptr;
}

compat::Expected<compat::Unit, compat::WebTransportError>
PicoquicWebTransport::sendDatagram(std::unique_ptr<compat::Payload> data) {
  if (!data || data->empty()) {
    return compat::makeUnexpected(compat::WebTransportError::SEND_ERROR);
  }

  int ret = picoquic_queue_datagram_frame(
      cnx_, data->size(), const_cast<uint8_t*>(data->data()));

  if (ret != 0) {
    return compat::makeUnexpected(compat::WebTransportError::SEND_ERROR);
  }

  return compat::Unit{};
}

void PicoquicWebTransport::setMaxDatagramSize(size_t maxSize) {
  maxDatagramSize_ = maxSize;
}

size_t PicoquicWebTransport::getMaxDatagramSize() const {
  return maxDatagramSize_;
}

void PicoquicWebTransport::closeSession(uint32_t errorCode) {
  if (!closed_) {
    closed_ = true;
    picoquic_close(cnx_, errorCode);
  }
}

void PicoquicWebTransport::drainSession() {
  // picoquic doesn't have a direct drain - just close gracefully
  closeSession(0);
}

compat::SocketAddress PicoquicWebTransport::getPeerAddress() const {
  struct sockaddr* addr = nullptr;
  picoquic_get_peer_addr(cnx_, &addr);
  if (addr) {
    return compat::SocketAddress(addr);
  }
  return compat::SocketAddress();
}

compat::SocketAddress PicoquicWebTransport::getLocalAddress() const {
  struct sockaddr* addr = nullptr;
  picoquic_get_local_addr(cnx_, &addr);
  if (addr) {
    return compat::SocketAddress(addr);
  }
  return compat::SocketAddress();
}

std::string PicoquicWebTransport::getALPN() const {
  const char* alpn = picoquic_tls_get_negotiated_alpn(cnx_);
  if (alpn) {
    return std::string(alpn);
  }
  return "";
}

bool PicoquicWebTransport::isConnected() const {
  return !closed_ &&
         picoquic_get_cnx_state(cnx_) == picoquic_state_ready;
}

void PicoquicWebTransport::setNewUniStreamCallback(
    std::function<void(compat::StreamReadHandle*)> cb) {
  newUniStreamCb_ = std::move(cb);
}

void PicoquicWebTransport::setNewBidiStreamCallback(
    std::function<void(compat::BidiStreamHandle*)> cb) {
  newBidiStreamCb_ = std::move(cb);
}

void PicoquicWebTransport::setDatagramCallback(
    std::function<void(std::unique_ptr<compat::Payload>)> cb) {
  datagramCb_ = std::move(cb);
}

void PicoquicWebTransport::setSessionCloseCallback(
    std::function<void(std::optional<uint32_t> error)> cb) {
  sessionCloseCb_ = std::move(cb);
}

void PicoquicWebTransport::setSessionDrainCallback(std::function<void()> cb) {
  sessionDrainCb_ = std::move(cb);
}

// --- PicoquicStreamWriteHandle Implementation ---

PicoquicStreamWriteHandle::PicoquicStreamWriteHandle(
    picoquic_cnx_t* cnx,
    uint64_t streamId)
    : cnx_(cnx), streamId_(streamId) {}

uint64_t PicoquicStreamWriteHandle::getID() const {
  return streamId_;
}

void PicoquicStreamWriteHandle::writeStreamData(
    std::unique_ptr<compat::Payload> data,
    bool fin,
    std::function<void(bool success)> callback) {
  auto result = writeStreamDataSync(std::move(data), fin);
  if (callback) {
    callback(result.hasValue());
  }
}

compat::Expected<compat::Unit, compat::WebTransportError>
PicoquicStreamWriteHandle::writeStreamDataSync(
    std::unique_ptr<compat::Payload> data,
    bool fin) {
  const uint8_t* bytes = data ? data->data() : nullptr;
  size_t length = data ? data->size() : 0;

  int ret = picoquic_add_to_stream(cnx_, streamId_, bytes, length, fin ? 1 : 0);

  if (ret != 0) {
    return compat::makeUnexpected(compat::WebTransportError::SEND_ERROR);
  }

  return compat::Unit{};
}

void PicoquicStreamWriteHandle::resetStream(uint32_t errorCode) {
  picoquic_reset_stream(cnx_, streamId_, errorCode);
}

void PicoquicStreamWriteHandle::setPriority(
    const compat::StreamPriority& /*priority*/) {
  // picoquic doesn't have direct priority API like this
  // Would need to use scheduling hints
}

void PicoquicStreamWriteHandle::awaitWritable(std::function<void()> callback) {
  writableCb_ = std::move(callback);
  // In picoquic, flow control is handled internally
  // For now, assume always writable and call immediately
  if (writableCb_) {
    writableCb_();
  }
}

void PicoquicStreamWriteHandle::setPeerCancelCallback(
    std::function<void(uint32_t)> cb) {
  cancelCb_ = std::move(cb);
}

compat::Expected<compat::Unit, compat::WebTransportError>
PicoquicStreamWriteHandle::registerDeliveryCallback(
    uint64_t /*offset*/,
    compat::DeliveryCallback* /*cb*/) {
  // picoquic doesn't have per-offset delivery callbacks
  // Would need custom tracking
  return compat::Unit{};
}

bool PicoquicStreamWriteHandle::isCancelled() const {
  return cancelled_;
}

std::optional<uint32_t> PicoquicStreamWriteHandle::getCancelError() const {
  return cancelError_;
}

// --- PicoquicStreamReadHandle Implementation ---

PicoquicStreamReadHandle::PicoquicStreamReadHandle(
    picoquic_cnx_t* cnx,
    uint64_t streamId)
    : cnx_(cnx), streamId_(streamId) {}

uint64_t PicoquicStreamReadHandle::getID() const {
  return streamId_;
}

void PicoquicStreamReadHandle::setReadCallback(
    std::function<void(compat::StreamData, std::optional<uint32_t> error)> cb) {
  readCb_ = std::move(cb);
}

void PicoquicStreamReadHandle::pauseReading() {
  paused_ = true;
}

void PicoquicStreamReadHandle::resumeReading() {
  paused_ = false;
}

compat::Expected<compat::Unit, compat::WebTransportError>
PicoquicStreamReadHandle::stopSending(uint32_t error) {
  picoquic_stop_sending(cnx_, streamId_, error);
  return compat::Unit{};
}

bool PicoquicStreamReadHandle::isFinished() const {
  return finished_;
}

void PicoquicStreamReadHandle::onData(
    const uint8_t* data,
    size_t length,
    bool fin) {
  if (fin) {
    finished_ = true;
  }

  if (!paused_ && readCb_) {
    compat::StreamData streamData;
    if (data && length > 0) {
      streamData.data = std::make_unique<compat::Payload>(data, data + length);
    }
    streamData.fin = fin;
    readCb_(std::move(streamData), std::nullopt);
  }
}

void PicoquicStreamReadHandle::onError(uint32_t errorCode) {
  finished_ = true;
  if (readCb_) {
    compat::StreamData streamData;
    readCb_(std::move(streamData), errorCode);
  }
}

// --- PicoquicBidiStreamHandle Implementation ---

PicoquicBidiStreamHandle::PicoquicBidiStreamHandle(
    std::unique_ptr<PicoquicStreamWriteHandle> write,
    std::unique_ptr<PicoquicStreamReadHandle> read)
    : writeHandle_(std::move(write)), readHandle_(std::move(read)) {}

} // namespace moxygen::transports

#endif // MOXYGEN_QUIC_PICOQUIC
