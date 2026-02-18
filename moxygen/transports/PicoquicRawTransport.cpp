/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "moxygen/transports/PicoquicRawTransport.h"

#if MOXYGEN_QUIC_PICOQUIC

#include <moxygen/compat/Expected.h>
#include <moxygen/compat/SocketAddress.h>
#include <moxygen/compat/Unit.h>
#include <cstdio>
#include <cstring>

namespace moxygen::transports {

// --- PicoquicThreadDispatcher Implementation ---

void PicoquicThreadDispatcher::setThreadContext(
    picoquic_network_thread_ctx_t* ctx) {
  threadCtx_ = ctx;
}

void PicoquicThreadDispatcher::setPicoThreadId(std::thread::id id) {
  picoThreadId_ = id;
}

bool PicoquicThreadDispatcher::isOnPicoThread() const {
  return std::this_thread::get_id() == picoThreadId_;
}

void PicoquicThreadDispatcher::runOnPicoThread(std::function<void()> fn) {
  if (isOnPicoThread()) {
    fn();
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    workQueue_.push_back(std::move(fn));
  }
  if (threadCtx_) {
    picoquic_wake_up_network_thread(threadCtx_);
  }
}

void PicoquicThreadDispatcher::drainWorkQueue() {
  std::deque<std::function<void()>> pending;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    pending.swap(workQueue_);
  }
  for (auto& fn : pending) {
    fn();
  }
}

// --- PicoquicRawTransport Implementation ---

PicoquicRawTransport::PicoquicRawTransport(
    picoquic_cnx_t* cnx,
    bool isServer,
    PicoquicThreadDispatcher* dispatcher)
    : cnx_(cnx), isServer_(isServer), dispatcher_(dispatcher) {
  // Note: we no longer call picoquic_set_callback here.
  // The server/client sets the callback after creating the transport
  // so it can pass the transport as callback_ctx.
}

PicoquicRawTransport::~PicoquicRawTransport() {
  if (!closed_) {
    closeSession(0);
  }
}

int PicoquicRawTransport::picoCallback(
    picoquic_cnx_t* cnx,
    uint64_t stream_id,
    uint8_t* bytes,
    size_t length,
    picoquic_call_back_event_t fin_or_event,
    void* callback_ctx,
    void* stream_ctx) {
  auto* self = static_cast<PicoquicRawTransport*>(callback_ctx);
  if (!self) {
    return 0;
  }

  return self->onStreamEvent(
      stream_id, bytes, length, fin_or_event, stream_ctx);
}

int PicoquicRawTransport::onStreamEvent(
    uint64_t streamId,
    uint8_t* bytes,
    size_t length,
    picoquic_call_back_event_t event,
    void* streamCtx) {
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
            static_cast<PicoquicStreamReadHandle*>(
                bidiIt->second->readHandle());
        readHandle->onData(bytes, length, fin);
        return 0;
      }

      // New stream - determine type and create handle
      bool isBidi = (streamId & 2) == 0;

      if (isBidi) {
        // New bidirectional stream
        auto write = std::make_unique<PicoquicStreamWriteHandle>(
            cnx_, streamId, dispatcher_);
        auto read = std::make_unique<PicoquicStreamReadHandle>(
            cnx_, streamId, dispatcher_);
        auto* readPtr = read.get();

        auto bidi = std::make_unique<PicoquicBidiStreamHandle>(
            std::move(write), std::move(read));
        auto* bidiPtr = bidi.get();
        bidiStreams_[streamId] = std::move(bidi);

        // Fire new stream callback BEFORE delivering data, so the
        // application can set the read callback before data arrives
        if (newBidiStreamCb_) {
          newBidiStreamCb_(bidiPtr);
        }

        readPtr->onData(bytes, length, fin);
      } else {
        // New unidirectional stream
        auto read = std::make_unique<PicoquicStreamReadHandle>(
            cnx_, streamId, dispatcher_);
        auto* readPtr = read.get();
        readStreams_[streamId] = std::move(read);

        // Fire new stream callback BEFORE delivering data, so the
        // application can set the read callback before data arrives
        if (newUniStreamCb_) {
          newUniStreamCb_(readPtr);
        }

        readPtr->onData(bytes, length, fin);
      }
      break;
    }

    case picoquic_callback_prepare_to_send: {
      // JIT send: picoquic is ready to send data on this stream.
      // stream_ctx was set to PicoquicStreamWriteHandle* by
      // picoquic_mark_active_stream
      auto* writeHandle =
          static_cast<PicoquicStreamWriteHandle*>(streamCtx);
      if (writeHandle) {
        return writeHandle->onPrepareToSend(bytes, length);
      }
      // Renege if handle not found
      picoquic_provide_stream_data_buffer(bytes, 0, 0, 0);
      break;
    }

    case picoquic_callback_datagram:
      onDatagram(bytes, length);
      break;

    case picoquic_callback_prepare_datagram:
      return onPrepareDatagram(bytes, length);

    case picoquic_callback_stream_reset: {
      // Stream was reset by peer
      auto readIt = readStreams_.find(streamId);
      if (readIt != readStreams_.end()) {
        readIt->second->onError(0); // TODO: get actual error code
      }
      auto bidiIt = bidiStreams_.find(streamId);
      if (bidiIt != bidiStreams_.end()) {
        auto* readHandle =
            static_cast<PicoquicStreamReadHandle*>(
                bidiIt->second->readHandle());
        readHandle->onError(0);
      }
      break;
    }

    case picoquic_callback_stop_sending: {
      // Peer sent STOP_SENDING
      auto writeIt = writeStreams_.find(streamId);
      if (writeIt != writeStreams_.end()) {
        // TODO: invoke cancel callback on the write handle
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

void PicoquicRawTransport::onDatagram(uint8_t* bytes, size_t length) {
  if (datagramCb_ && bytes && length > 0) {
    auto payload = compat::Payload::copyBuffer(bytes, length);
    datagramCb_(std::move(payload));
  }
}

int PicoquicRawTransport::onPrepareDatagram(
    uint8_t* context,
    size_t maxLength) {
  std::lock_guard<std::mutex> lock(datagramMutex_);
  if (datagramQueue_.empty()) {
    // Nothing to send, mark not ready
    picoquic_provide_datagram_buffer(context, 0);
    return 0;
  }

  auto& front = datagramQueue_.front();
  size_t dgLen = front->size();
  if (dgLen > maxLength) {
    // Datagram too large for this packet, try again in next packet
    picoquic_provide_datagram_buffer_ex(
        context, 0, picoquic_datagram_active_any_path);
    return 0;
  }

  bool moreReady = datagramQueue_.size() > 1;
  uint8_t* buf;
  if (moreReady) {
    buf = picoquic_provide_datagram_buffer_ex(
        context, dgLen, picoquic_datagram_active_any_path);
  } else {
    buf = picoquic_provide_datagram_buffer(context, dgLen);
  }
  if (buf) {
    memcpy(buf, front->data(), dgLen);
    datagramQueue_.pop_front();
  }

  return 0;
}

compat::Expected<compat::StreamWriteHandle*, compat::WebTransportError>
PicoquicRawTransport::createUniStream() {
  using ResultType =
      compat::Expected<compat::StreamWriteHandle*, compat::WebTransportError>;

  if (dispatcher_) {
    return dispatcher_->runOnPicoThreadSync<ResultType>(
        [this]() -> ResultType {
          // is_unidir=true for unidirectional streams
          uint64_t streamId =
              picoquic_get_next_local_stream_id(cnx_, true);
          auto handle = std::make_unique<PicoquicStreamWriteHandle>(
              cnx_, streamId, dispatcher_);
          auto* ptr = handle.get();
          writeStreams_[streamId] = std::move(handle);
          return ptr;
        });
  }

  // Fallback if no dispatcher (should not happen in practice)
  uint64_t streamId = picoquic_get_next_local_stream_id(cnx_, true);
  auto handle = std::make_unique<PicoquicStreamWriteHandle>(
      cnx_, streamId, dispatcher_);
  auto* ptr = handle.get();
  writeStreams_[streamId] = std::move(handle);
  return ptr;
}

compat::Expected<compat::BidiStreamHandle*, compat::WebTransportError>
PicoquicRawTransport::createBidiStream() {
  using ResultType =
      compat::Expected<compat::BidiStreamHandle*, compat::WebTransportError>;

  if (dispatcher_) {
    return dispatcher_->runOnPicoThreadSync<ResultType>(
        [this]() -> ResultType {
          // is_unidir=false for bidirectional streams
          uint64_t streamId =
              picoquic_get_next_local_stream_id(cnx_, false);
          auto write = std::make_unique<PicoquicStreamWriteHandle>(
              cnx_, streamId, dispatcher_);
          auto read = std::make_unique<PicoquicStreamReadHandle>(
              cnx_, streamId, dispatcher_);
          auto bidi = std::make_unique<PicoquicBidiStreamHandle>(
              std::move(write), std::move(read));
          auto* ptr = bidi.get();
          bidiStreams_[streamId] = std::move(bidi);
          return ptr;
        });
  }

  // Fallback if no dispatcher
  uint64_t streamId = picoquic_get_next_local_stream_id(cnx_, false);
  auto write = std::make_unique<PicoquicStreamWriteHandle>(
      cnx_, streamId, dispatcher_);
  auto read = std::make_unique<PicoquicStreamReadHandle>(
      cnx_, streamId, dispatcher_);
  auto bidi = std::make_unique<PicoquicBidiStreamHandle>(
      std::move(write), std::move(read));
  auto* ptr = bidi.get();
  bidiStreams_[streamId] = std::move(bidi);
  return ptr;
}

compat::Expected<compat::Unit, compat::WebTransportError>
PicoquicRawTransport::sendDatagram(std::unique_ptr<compat::Payload> data) {
  if (!data || data->empty()) {
    return compat::makeUnexpected(compat::WebTransportError::SEND_ERROR);
  }

  // Buffer the datagram
  {
    std::lock_guard<std::mutex> lock(datagramMutex_);
    datagramQueue_.push_back(std::move(data));
  }

  // Mark datagrams ready on the picoquic thread
  if (dispatcher_) {
    if (dispatcher_->isOnPicoThread()) {
      picoquic_mark_datagram_ready(cnx_, 1);
    } else {
      auto cnx = cnx_;
      dispatcher_->runOnPicoThread([cnx]() {
        picoquic_mark_datagram_ready(cnx, 1);
      });
    }
  }

  return compat::Unit{};
}

void PicoquicRawTransport::setMaxDatagramSize(size_t maxSize) {
  maxDatagramSize_ = maxSize;
}

size_t PicoquicRawTransport::getMaxDatagramSize() const {
  return maxDatagramSize_;
}

void PicoquicRawTransport::closeSession(uint32_t errorCode) {
  if (!closed_) {
    closed_ = true;
    if (dispatcher_ && !dispatcher_->isOnPicoThread()) {
      auto cnx = cnx_;
      dispatcher_->runOnPicoThread([cnx, errorCode]() {
        picoquic_close(cnx, errorCode);
      });
    } else {
      picoquic_close(cnx_, errorCode);
    }
  }
}

void PicoquicRawTransport::drainSession() {
  // picoquic doesn't have a direct drain - just close gracefully
  closeSession(0);
}

compat::SocketAddress PicoquicRawTransport::getPeerAddress() const {
  struct sockaddr* addr = nullptr;
  picoquic_get_peer_addr(cnx_, &addr);
  return compat::makeSocketAddress(addr);
}

compat::SocketAddress PicoquicRawTransport::getLocalAddress() const {
  struct sockaddr* addr = nullptr;
  picoquic_get_local_addr(cnx_, &addr);
  return compat::makeSocketAddress(addr);
}

std::string PicoquicRawTransport::getALPN() const {
  const char* alpn = picoquic_tls_get_negotiated_alpn(cnx_);
  if (alpn) {
    return std::string(alpn);
  }
  return "";
}

bool PicoquicRawTransport::isConnected() const {
  return !closed_ &&
         picoquic_get_cnx_state(cnx_) == picoquic_state_ready;
}

void PicoquicRawTransport::setNewUniStreamCallback(
    std::function<void(compat::StreamReadHandle*)> cb) {
  newUniStreamCb_ = std::move(cb);
}

void PicoquicRawTransport::setNewBidiStreamCallback(
    std::function<void(compat::BidiStreamHandle*)> cb) {
  newBidiStreamCb_ = std::move(cb);
}

void PicoquicRawTransport::setDatagramCallback(
    std::function<void(std::unique_ptr<compat::Payload>)> cb) {
  datagramCb_ = std::move(cb);
}

void PicoquicRawTransport::setSessionCloseCallback(
    std::function<void(std::optional<uint32_t> error)> cb) {
  sessionCloseCb_ = std::move(cb);
}

void PicoquicRawTransport::setSessionDrainCallback(
    std::function<void()> cb) {
  sessionDrainCb_ = std::move(cb);
}

// --- PicoquicStreamWriteHandle Implementation ---

PicoquicStreamWriteHandle::PicoquicStreamWriteHandle(
    picoquic_cnx_t* cnx,
    uint64_t streamId,
    PicoquicThreadDispatcher* dispatcher)
    : cnx_(cnx), streamId_(streamId), dispatcher_(dispatcher) {}

uint64_t PicoquicStreamWriteHandle::getID() const {
  return streamId_;
}

void PicoquicStreamWriteHandle::markActive() {
  if (dispatcher_ && !dispatcher_->isOnPicoThread()) {
    auto cnx = cnx_;
    auto streamId = streamId_;
    auto self = this;
    dispatcher_->runOnPicoThread([cnx, streamId, self]() {
      picoquic_mark_active_stream(cnx, streamId, 1, self);
    });
  } else {
    picoquic_mark_active_stream(cnx_, streamId_, 1, this);
  }
}

void PicoquicStreamWriteHandle::writeStreamData(
    std::unique_ptr<compat::Payload> data,
    bool fin,
    std::function<void(bool success)> callback) {
  // Buffer the data
  {
    std::lock_guard<std::mutex> lock(outbuf_.mutex);
    if (data && !data->empty()) {
      outbuf_.chunks.push_back(std::move(data));
    }
    if (fin) {
      outbuf_.finQueued = true;
    }
  }

  // Mark stream active so picoquic will call prepare_to_send
  markActive();

  if (callback) {
    callback(true);
  }
}

compat::Expected<compat::Unit, compat::WebTransportError>
PicoquicStreamWriteHandle::writeStreamDataSync(
    std::unique_ptr<compat::Payload> data,
    bool fin) {
  // Buffer the data
  {
    std::lock_guard<std::mutex> lock(outbuf_.mutex);
    if (data && !data->empty()) {
      outbuf_.chunks.push_back(std::move(data));
    }
    if (fin) {
      outbuf_.finQueued = true;
    }
  }

  // Mark stream active so picoquic will call prepare_to_send
  markActive();

  return compat::Unit{};
}

int PicoquicStreamWriteHandle::onPrepareToSend(
    void* context,
    size_t maxLength) {
  // Collect waiters to signal outside the lock
  std::vector<compat::Promise<compat::Unit>> waitersToSignal;

  {
    std::lock_guard<std::mutex> lock(outbuf_.mutex);

    // Signal any waiters that picoquic is ready for data
    // This is the key JIT backpressure signal
    waitersToSignal = std::move(outbuf_.readyWaiters);
    outbuf_.readyWaiters.clear();

    // Calculate available data
    size_t available = 0;
    for (auto& chunk : outbuf_.chunks) {
      available += chunk->size();
    }
    available -= outbuf_.firstChunkOffset;

    if (available == 0 && !outbuf_.finQueued) {
      // Nothing to send — renege
      picoquic_provide_stream_data_buffer(context, 0, 0, 0);
      // Still signal waiters so publishers can queue more data
      goto signal_waiters;
    }

    if (available == 0 && outbuf_.finQueued && !outbuf_.finSent) {
      // Only FIN to send, no data
      picoquic_provide_stream_data_buffer(context, 0, 1, 0);
      outbuf_.finSent = true;
      goto signal_waiters;
    }

    size_t toSend = std::min(available, maxLength);
    bool isFin = outbuf_.finQueued && (toSend == available);
    bool stillActive = (toSend < available);

    uint8_t* buf = picoquic_provide_stream_data_buffer(
        context, toSend, isFin ? 1 : 0, stillActive ? 1 : 0);

    if (!buf || toSend == 0) {
      goto signal_waiters;
    }

    // Copy data from chunks to picoquic buffer
    size_t written = 0;
    while (written < toSend && !outbuf_.chunks.empty()) {
      auto& front = outbuf_.chunks.front();
      size_t chunkRemaining = front->size() - outbuf_.firstChunkOffset;
      size_t copyLen = std::min(chunkRemaining, toSend - written);

      memcpy(buf + written, front->data() + outbuf_.firstChunkOffset, copyLen);
      written += copyLen;
      outbuf_.firstChunkOffset += copyLen;

      if (outbuf_.firstChunkOffset >= front->size()) {
        outbuf_.chunks.pop_front();
        outbuf_.firstChunkOffset = 0;
      }
    }

    if (isFin) {
      outbuf_.finSent = true;
    }
  }

signal_waiters:
  // Signal waiters outside the lock to prevent deadlock
  for (auto& promise : waitersToSignal) {
    if (!promise.isFulfilled()) {
      promise.setValue(compat::unit);
    }
  }

  return 0;
}

void PicoquicStreamWriteHandle::resetStream(uint32_t errorCode) {
  if (dispatcher_ && !dispatcher_->isOnPicoThread()) {
    auto cnx = cnx_;
    auto streamId = streamId_;
    dispatcher_->runOnPicoThread([cnx, streamId, errorCode]() {
      picoquic_reset_stream(cnx, streamId, errorCode);
    });
  } else {
    picoquic_reset_stream(cnx_, streamId_, errorCode);
  }
}

void PicoquicStreamWriteHandle::setPriority(
    const compat::StreamPriority& /*priority*/) {
  // picoquic doesn't have direct priority API like this
  // Would need to use scheduling hints
}

void PicoquicStreamWriteHandle::awaitWritable(
    std::function<void()> callback) {
  writableCb_ = std::move(callback);
  // With JIT API, the stream is always "writable" from the app's perspective
  // (data is buffered and sent when flow credits are available)
  if (writableCb_) {
    writableCb_();
  }
}

compat::Expected<compat::SemiFuture<compat::Unit>, compat::WebTransportError>
PicoquicStreamWriteHandle::awaitWritable() {
  std::lock_guard<std::mutex> lock(outbuf_.mutex);

  // Create a promise and store it - will be fulfilled when prepare_to_send fires
  outbuf_.readyWaiters.emplace_back();
  return outbuf_.readyWaiters.back().getSemiFuture();
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
    uint64_t streamId,
    PicoquicThreadDispatcher* dispatcher)
    : cnx_(cnx), streamId_(streamId), dispatcher_(dispatcher) {}

uint64_t PicoquicStreamReadHandle::getID() const {
  return streamId_;
}

void PicoquicStreamReadHandle::setReadCallback(
    std::function<void(compat::StreamData, std::optional<uint32_t> error)>
        cb) {
  readCb_ = std::move(cb);
  // Deliver any data that was buffered before the callback was set
  deliverBuffered();
}

void PicoquicStreamReadHandle::pauseReading() {
  paused_ = true;
}

void PicoquicStreamReadHandle::resumeReading() {
  paused_ = false;
}

compat::Expected<compat::Unit, compat::WebTransportError>
PicoquicStreamReadHandle::stopSending(uint32_t error) {
  if (dispatcher_ && !dispatcher_->isOnPicoThread()) {
    using ResultType =
        compat::Expected<compat::Unit, compat::WebTransportError>;
    auto cnx = cnx_;
    auto streamId = streamId_;
    return dispatcher_->runOnPicoThreadSync<ResultType>(
        [cnx, streamId, error]() -> ResultType {
          picoquic_stop_sending(cnx, streamId, error);
          return compat::Unit{};
        });
  }
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
      streamData.data = compat::Payload::copyBuffer(data, length);
    }
    streamData.fin = fin;
    readCb_(std::move(streamData), std::nullopt);
  } else if (!readCb_) {
    // Buffer data until read callback is set
    BufferedData bd;
    if (data && length > 0) {
      bd.data = compat::Payload::copyBuffer(data, length);
    }
    bd.fin = fin;
    buffered_.push_back(std::move(bd));
  }
}

void PicoquicStreamReadHandle::deliverBuffered() {
  if (!readCb_ || buffered_.empty()) {
    return;
  }
  auto pending = std::move(buffered_);
  buffered_.clear();
  for (auto& bd : pending) {
    if (readCb_) {
      compat::StreamData streamData;
      streamData.data = std::move(bd.data);
      streamData.fin = bd.fin;
      readCb_(std::move(streamData), std::nullopt);
    }
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
