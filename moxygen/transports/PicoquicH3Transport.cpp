/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <moxygen/transports/PicoquicH3Transport.h>

#if MOXYGEN_QUIC_PICOQUIC

#include <moxygen/compat/Payload.h>

#include <cstring>
#include <iomanip>
#include <iostream>

// WebTransport API from picoquic
extern "C" {
#include <democlient.h>
#include <demoserver.h>
#include <pico_webtransport.h>
#include <picoquic_internal.h>
}

namespace moxygen::transports {

// ============================================================================
// PicoquicH3Transport implementation
// ============================================================================

PicoquicH3Transport::PicoquicH3Transport(
    picoquic_cnx_t* cnx,
    const std::string& path,
    PicoquicThreadDispatcher* dispatcher)
    : cnx_(cnx),
      path_(path),
      isServer_(false),
      dispatcher_(dispatcher) {}

PicoquicH3Transport::PicoquicH3Transport(
    picoquic_cnx_t* cnx,
    h3zero_callback_ctx_t* h3Ctx,
    void* wtSessionCtx,
    PicoquicThreadDispatcher* dispatcher)
    : cnx_(cnx),
      isServer_(true),
      dispatcher_(dispatcher),
      h3Ctx_(h3Ctx),
      ownsH3Ctx_(false),
      wtSessionCtx_(wtSessionCtx),
      wtReady_(true) {
  // For server-side, wtSessionCtx is the stream context from the CONNECT request
  // Set controlStreamId_ from the CONNECT stream ID
  if (wtSessionCtx) {
    auto* streamCtx = static_cast<h3zero_stream_ctx_t*>(wtSessionCtx);
    controlStreamId_ = streamCtx->stream_id;
    controlStreamCtx_ = streamCtx;
    std::cerr << "[H3T] Server transport created with controlStreamId=" << controlStreamId_ << "\n";
  }
}

PicoquicH3Transport::~PicoquicH3Transport() {
  if (ownsH3Ctx_ && h3Ctx_ && cnx_) {
    h3zero_callback_delete_context(cnx_, h3Ctx_);
  }
}

bool PicoquicH3Transport::initH3Context() {
  if (h3Ctx_) {
    return true;  // Already initialized
  }

  // Create H3 callback context
  h3Ctx_ = h3zero_callback_create_context(nullptr);
  if (!h3Ctx_) {
    return false;
  }
  ownsH3Ctx_ = true;

  // Set up the callback
  picoquic_set_callback(cnx_, &PicoquicH3Transport::quicCallback, this);

  return true;
}

bool PicoquicH3Transport::initiateWebTransportUpgrade() {
  std::cerr << "[H3T] initiateWebTransportUpgrade called\n";
  if (!h3Ctx_ || !cnx_) {
    std::cerr << "[H3T] h3Ctx_ or cnx_ is null\n";
    return false;
  }

  // Set WebTransport transport parameters
  picowt_set_transport_parameters(cnx_);

  // Create control stream for WebTransport session
  controlStreamCtx_ = picowt_set_control_stream(cnx_, h3Ctx_);
  if (!controlStreamCtx_) {
    std::cerr << "[H3T] initiateWebTransportUpgrade: picowt_set_control_stream failed!\n";
    return false;
  }
  controlStreamId_ = controlStreamCtx_->stream_id;
  std::cerr << "[H3T] initiateWebTransportUpgrade: controlStreamId=" << controlStreamId_ << "\n";

  // Extract authority from connection (SNI)
  const char* sni = picoquic_tls_get_sni(cnx_);
  authority_ = sni ? sni : "localhost";

  // Send CONNECT request to establish WebTransport session
  int ret = picowt_connect(
      cnx_,
      h3Ctx_,
      controlStreamCtx_,
      authority_.c_str(),
      path_.c_str(),
      &PicoquicH3Transport::wtCallback,
      this);

  return ret == 0;
}

int PicoquicH3Transport::quicCallback(
    picoquic_cnx_t* cnx,
    uint64_t stream_id,
    uint8_t* bytes,
    size_t length,
    picoquic_call_back_event_t event,
    void* callback_ctx,
    void* stream_ctx) {
  auto* transport = static_cast<PicoquicH3Transport*>(callback_ctx);
  if (!transport || !transport->h3Ctx_) {
    std::cerr << "[H3T] quicCallback: transport or h3Ctx_ is null\n";
    return -1;
  }

  std::cerr << "[H3T] quicCallback: event=" << event << " stream_id=" << stream_id << "\n";

  // Handle connection-level events before delegating to h3zero
  switch (event) {
    case picoquic_callback_ready:
      std::cerr << "[H3T] picoquic_callback_ready\n";
      if (transport->connectionReadyCb_) {
        transport->connectionReadyCb_();
      }
      // Fall through to h3zero callback for any h3 setup
      break;

    case picoquic_callback_close:
    case picoquic_callback_application_close:
      std::cerr << "[H3T] picoquic_callback_close/application_close\n";
      if (transport->connectionCloseCb_) {
        transport->connectionCloseCb_(0);
      }
      // Fall through to h3zero callback for cleanup
      break;

    default:
      break;
  }

  // Delegate to h3zero callback
  int ret = h3zero_callback(
      cnx, stream_id, bytes, length, event, transport->h3Ctx_, stream_ctx);
  std::cerr << "[H3T] h3zero_callback returned: " << ret << "\n";
  return ret;
}

int PicoquicH3Transport::h3Callback(
    picoquic_cnx_t* cnx,
    uint64_t stream_id,
    uint8_t* bytes,
    size_t length,
    picohttp_call_back_event_t event,
    h3zero_stream_ctx_t* stream_ctx,
    void* callback_ctx) {
  auto* transport = static_cast<PicoquicH3Transport*>(callback_ctx);
  if (!transport) {
    return -1;
  }
  return transport->onH3Event(stream_id, bytes, length, event, stream_ctx);
}

int PicoquicH3Transport::wtCallback(
    picoquic_cnx_t* cnx,
    uint8_t* bytes,
    size_t length,
    picohttp_call_back_event_t event,
    h3zero_stream_ctx_t* stream_ctx,
    void* callback_ctx) {
  auto* transport = static_cast<PicoquicH3Transport*>(callback_ctx);
  if (!transport) {
    return -1;
  }

  uint64_t streamId = stream_ctx ? stream_ctx->stream_id : UINT64_MAX;
  return transport->onH3Event(streamId, bytes, length, event, stream_ctx);
}

int PicoquicH3Transport::onH3Event(
    uint64_t streamId,
    uint8_t* bytes,
    size_t length,
    picohttp_call_back_event_t event,
    h3zero_stream_ctx_t* streamCtx) {
  std::cerr << "[H3T] onH3Event event=" << event << " streamId=" << streamId
            << " length=" << length << " isServer=" << isServer_ << "\n";
  switch (event) {
    case picohttp_callback_connecting:
      std::cerr << "[H3T] picohttp_callback_connecting\n";
      // CONNECT request is being sent
      break;

    case picohttp_callback_connect_accepted:
      std::cerr << "[H3T] picohttp_callback_connect_accepted\n";
      onConnectAccepted();
      break;

    case picohttp_callback_connect_refused:
      std::cerr << "[H3T] picohttp_callback_connect_refused\n";
      onConnectRefused();
      break;

    case picohttp_callback_post_data:
    case picohttp_callback_post_fin: {
      // Data received on a stream
      bool isFin = (event == picohttp_callback_post_fin);
      std::cerr << "[H3T] post_data/fin: streamId=" << streamId
                << " controlStreamId=" << controlStreamId_
                << " length=" << length << " isFin=" << isFin << "\n";

      // Check if this is a new incoming stream we haven't seen
      if (streamId != controlStreamId_) {
        auto readIt = readStreams_.find(streamId);
        auto bidiIt = bidiStreams_.find(streamId);

        if (readIt == readStreams_.end() && bidiIt == bidiStreams_.end()) {
          // New incoming stream - create handles and notify callback
          bool isUnidir = (streamId & 0x02) != 0;
          std::cerr << "[H3T] Creating new stream: " << streamId
                    << " isUnidir=" << isUnidir << "\n";
          onNewWtStream(streamId, isUnidir);
        }

        // Now deliver the data - check readStreams_ first, then bidiStreams_
        readIt = readStreams_.find(streamId);
        if (readIt != readStreams_.end()) {
          readIt->second->onData(bytes, length, isFin);
        } else {
          // For bidi streams, the read handle is inside the bidi handle
          bidiIt = bidiStreams_.find(streamId);
          if (bidiIt != bidiStreams_.end()) {
            auto* readHandle = static_cast<PicoquicH3StreamReadHandle*>(
                bidiIt->second->readHandle());
            if (readHandle) {
              readHandle->onData(bytes, length, isFin);
            }
          }
        }
      }
      break;
    }

    case picohttp_callback_provide_data: {
      // Ready to send data - check for write handles
      auto writeIt = writeStreams_.find(streamId);
      if (writeIt != writeStreams_.end()) {
        return writeIt->second->onPrepareToSend(bytes, length);
      }
      // Also check bidi streams (write handles are stored there too)
      auto bidiIt = bidiStreams_.find(streamId);
      if (bidiIt != bidiStreams_.end()) {
        auto* writeHandle = static_cast<PicoquicH3StreamWriteHandle*>(
            bidiIt->second->writeHandle());
        if (writeHandle) {
          return writeHandle->onPrepareToSend(bytes, length);
        }
      }
      break;
    }

    case picohttp_callback_post_datagram:
      onDatagram(bytes, length);
      break;

    case picohttp_callback_provide_datagram:
      return onProvideDatagram(bytes, length);
      break;

    case picohttp_callback_reset: {
      auto it = readStreams_.find(streamId);
      if (it != readStreams_.end()) {
        // Extract error code from the underlying picoquic stream
        uint64_t errorCode = 0;
        picoquic_stream_head_t* stream = picoquic_find_stream(cnx_, streamId);
        if (stream) {
          errorCode = stream->remote_error;
        }
        it->second->onError(static_cast<uint32_t>(errorCode));
      }
      break;
    }

    case picohttp_callback_stop_sending: {
      // Peer wants us to stop sending
      auto it = writeStreams_.find(streamId);
      if (it != writeStreams_.end()) {
        // Extract error code from the underlying picoquic stream
        uint64_t errorCode = 0;
        picoquic_stream_head_t* stream = picoquic_find_stream(cnx_, streamId);
        if (stream) {
          errorCode = stream->remote_stop_error;
        }
        it->second->onStopSending(static_cast<uint32_t>(errorCode));
      }
      break;
    }

    case picohttp_callback_deregister:
    case picohttp_callback_free:
      // Cleanup stream resources
      readStreams_.erase(streamId);
      writeStreams_.erase(streamId);
      bidiStreams_.erase(streamId);
      break;

    default:
      break;
  }
  return 0;
}

void PicoquicH3Transport::onConnectAccepted() {
  wtReady_ = true;
  if (wtReadyCb_) {
    wtReadyCb_(true);
  }
}

void PicoquicH3Transport::onConnectRefused() {
  wtReady_ = false;
  if (wtReadyCb_) {
    wtReadyCb_(false);
  }
  if (sessionCloseCb_) {
    sessionCloseCb_(1);  // Generic error
  }
}

void PicoquicH3Transport::onNewWtStream(
    uint64_t streamId,
    bool isUnidirectional) {
  std::cerr << "[H3T] onNewWtStream: streamId=" << streamId
            << " isUnidirectional=" << isUnidirectional
            << " hasBidiCb=" << (newBidiStreamCb_ ? "yes" : "no")
            << " hasUniCb=" << (newUniStreamCb_ ? "yes" : "no") << "\n";
  if (isUnidirectional) {
    // Unidirectional stream from peer - only readable
    auto readHandle = std::make_unique<PicoquicH3StreamReadHandle>(
        cnx_, streamId, h3Ctx_, dispatcher_);
    auto* ptr = readHandle.get();
    readStreams_[streamId] = std::move(readHandle);
    if (newUniStreamCb_) {
      newUniStreamCb_(ptr);
    }
  } else {
    // Bidirectional stream from peer - both readable and writable
    auto write = std::make_unique<PicoquicH3StreamWriteHandle>(
        cnx_, streamId, h3Ctx_, dispatcher_);
    auto read = std::make_unique<PicoquicH3StreamReadHandle>(
        cnx_, streamId, h3Ctx_, dispatcher_);
    auto* readPtr = read.get();

    auto bidi = std::make_unique<PicoquicH3BidiStreamHandle>(
        std::move(write), std::move(read));
    auto* ptr = bidi.get();
    bidiStreams_[streamId] = std::move(bidi);

    // Note: Don't store separate readHandle - look up from bidiStreams_ when delivering data
    // This ensures callbacks set on bh->readHandle() receive the delivered data

    if (newBidiStreamCb_) {
      newBidiStreamCb_(ptr);
    }
  }
}

void PicoquicH3Transport::onDatagram(const uint8_t* bytes, size_t length) {
  if (datagramCb_) {
    auto payload = compat::Payload::copyBuffer(bytes, length);
    datagramCb_(std::move(payload));
  }
}

int PicoquicH3Transport::onProvideDatagram(uint8_t* context, size_t maxLength) {
  std::lock_guard<std::mutex> lock(datagramMutex_);

  if (datagramQueue_.empty()) {
    // Nothing to send
    h3zero_provide_datagram_buffer(context, 0, 0);
    return 0;
  }

  auto& front = datagramQueue_.front();
  front->coalesce();
  size_t dgLen = front->computeChainDataLength();

  if (dgLen > maxLength) {
    // Datagram too large for available space, try again later
    h3zero_provide_datagram_buffer(context, 0, 1);
    return 0;
  }

  bool moreReady = datagramQueue_.size() > 1;
  uint8_t* buf = h3zero_provide_datagram_buffer(context, dgLen, moreReady ? 1 : 0);

  if (buf) {
    std::memcpy(buf, front->data(), dgLen);
    datagramQueue_.pop_front();
  }

  return 0;
}

// --- WebTransportInterface implementation ---

compat::Expected<compat::StreamWriteHandle*, compat::WebTransportError>
PicoquicH3Transport::createUniStream() {
  if (!wtReady_ || closed_ || controlStreamId_ == UINT64_MAX) {
    return compat::makeUnexpected(compat::WebTransportError::STREAM_CREATION_ERROR);
  }

  // Create WebTransport unidirectional stream using picowt API
  h3zero_stream_ctx_t* streamCtx = picowt_create_local_stream(
      cnx_, 0 /* unidirectional */, h3Ctx_, controlStreamId_);

  if (!streamCtx) {
    return compat::makeUnexpected(compat::WebTransportError::STREAM_CREATION_ERROR);
  }

  uint64_t streamId = streamCtx->stream_id;

  // Set up callback for this stream
  streamCtx->path_callback = &PicoquicH3Transport::wtCallback;
  streamCtx->path_callback_ctx = this;

  auto writeHandle = std::make_unique<PicoquicH3StreamWriteHandle>(
      cnx_, streamId, h3Ctx_, dispatcher_);
  auto* ptr = writeHandle.get();
  writeStreams_[streamId] = std::move(writeHandle);

  return ptr;
}

compat::Expected<compat::BidiStreamHandle*, compat::WebTransportError>
PicoquicH3Transport::createBidiStream() {
  std::cerr << "[H3T] createBidiStream: wtReady=" << wtReady_
            << " closed=" << closed_
            << " controlStreamId=" << controlStreamId_ << "\n";
  if (!wtReady_ || closed_ || controlStreamId_ == UINT64_MAX) {
    return compat::makeUnexpected(compat::WebTransportError::STREAM_CREATION_ERROR);
  }

  // Create WebTransport bidirectional stream using picowt API
  h3zero_stream_ctx_t* streamCtx = picowt_create_local_stream(
      cnx_, 1 /* bidirectional */, h3Ctx_, controlStreamId_);
  std::cerr << "[H3T] createBidiStream: picowt_create_local_stream returned "
            << (streamCtx ? "valid" : "NULL")
            << " streamId=" << (streamCtx ? streamCtx->stream_id : UINT64_MAX) << "\n";

  if (!streamCtx) {
    return compat::makeUnexpected(compat::WebTransportError::STREAM_CREATION_ERROR);
  }

  uint64_t streamId = streamCtx->stream_id;

  // Set up callback for this stream
  streamCtx->path_callback = &PicoquicH3Transport::wtCallback;
  streamCtx->path_callback_ctx = this;

  // Create write and read handles for the bidirectional stream
  auto write = std::make_unique<PicoquicH3StreamWriteHandle>(
      cnx_, streamId, h3Ctx_, dispatcher_);
  auto read = std::make_unique<PicoquicH3StreamReadHandle>(
      cnx_, streamId, h3Ctx_, dispatcher_);

  // Note: Don't store separate readHandle - look up from bidiStreams_ when delivering data
  // This ensures callbacks set on bh->readHandle() receive the delivered data

  auto bidi = std::make_unique<PicoquicH3BidiStreamHandle>(
      std::move(write), std::move(read));
  auto* ptr = bidi.get();
  bidiStreams_[streamId] = std::move(bidi);

  return ptr;
}

compat::Expected<compat::Unit, compat::WebTransportError>
PicoquicH3Transport::sendDatagram(std::unique_ptr<compat::Payload> data) {
  if (!wtReady_ || closed_ || !data) {
    return compat::makeUnexpected(compat::WebTransportError::SEND_ERROR);
  }

  size_t len = data->computeChainDataLength();
  if (len > maxDatagramSize_) {
    return compat::makeUnexpected(compat::WebTransportError::SEND_ERROR);
  }

  // Queue the datagram
  {
    std::lock_guard<std::mutex> lock(datagramMutex_);
    datagramQueue_.push_back(std::move(data));
  }

  // Signal readiness to send datagram via h3zero
  h3zero_set_datagram_ready(cnx_, controlStreamId_);
  return compat::unit;
}

void PicoquicH3Transport::setMaxDatagramSize(size_t maxSize) {
  maxDatagramSize_ = maxSize;
}

size_t PicoquicH3Transport::getMaxDatagramSize() const {
  return maxDatagramSize_;
}

void PicoquicH3Transport::closeSession(uint32_t errorCode) {
  if (closed_) {
    return;
  }
  closed_ = true;

  // Send WebTransport close capsule if we have a control stream
  if (controlStreamCtx_) {
    picowt_send_close_session_message(cnx_, controlStreamCtx_, errorCode, nullptr);
  } else {
    // Fall back to closing the QUIC connection
    picoquic_close(cnx_, errorCode);
  }

  if (sessionCloseCb_) {
    sessionCloseCb_(errorCode);
  }
}

void PicoquicH3Transport::drainSession() {
  // Send WebTransport drain capsule if we have a control stream
  if (controlStreamCtx_) {
    picowt_send_drain_session_message(cnx_, controlStreamCtx_);
  }

  if (sessionDrainCb_) {
    sessionDrainCb_();
  }
}

compat::SocketAddress PicoquicH3Transport::getPeerAddress() const {
  sockaddr_storage addr;
  picoquic_get_peer_addr(cnx_, reinterpret_cast<sockaddr**>(&addr));
  return compat::makeSocketAddress(reinterpret_cast<sockaddr*>(&addr));
}

compat::SocketAddress PicoquicH3Transport::getLocalAddress() const {
  sockaddr_storage addr;
  picoquic_get_local_addr(cnx_, reinterpret_cast<sockaddr**>(&addr));
  return compat::makeSocketAddress(reinterpret_cast<sockaddr*>(&addr));
}

std::string PicoquicH3Transport::getALPN() const {
  const char* alpn = picoquic_tls_get_negotiated_alpn(cnx_);
  if (alpn) {
    return std::string(alpn);
  }
  return "h3";  // Default for HTTP/3
}

bool PicoquicH3Transport::isConnected() const {
  if (!cnx_) {
    return false;
  }
  picoquic_state_enum state = picoquic_get_cnx_state(cnx_);
  return state == picoquic_state_ready;
}

void PicoquicH3Transport::setNewUniStreamCallback(
    std::function<void(compat::StreamReadHandle*)> cb) {
  newUniStreamCb_ = std::move(cb);
}

void PicoquicH3Transport::setNewBidiStreamCallback(
    std::function<void(compat::BidiStreamHandle*)> cb) {
  newBidiStreamCb_ = std::move(cb);
}

void PicoquicH3Transport::setDatagramCallback(
    std::function<void(std::unique_ptr<compat::Payload>)> cb) {
  datagramCb_ = std::move(cb);
}

void PicoquicH3Transport::setSessionCloseCallback(
    std::function<void(std::optional<uint32_t> error)> cb) {
  sessionCloseCb_ = std::move(cb);
}

void PicoquicH3Transport::setSessionDrainCallback(std::function<void()> cb) {
  sessionDrainCb_ = std::move(cb);
}

// ============================================================================
// PicoquicH3StreamWriteHandle implementation
// ============================================================================

PicoquicH3StreamWriteHandle::PicoquicH3StreamWriteHandle(
    picoquic_cnx_t* cnx,
    uint64_t streamId,
    h3zero_callback_ctx_t* h3Ctx,
    PicoquicThreadDispatcher* dispatcher)
    : cnx_(cnx),
      streamId_(streamId),
      h3Ctx_(h3Ctx),
      dispatcher_(dispatcher) {}

uint64_t PicoquicH3StreamWriteHandle::getID() const {
  return streamId_;
}

void PicoquicH3StreamWriteHandle::writeStreamData(
    std::unique_ptr<compat::Payload> data,
    bool fin,
    std::function<void(bool success)> callback) {
  {
    std::lock_guard<std::mutex> lock(outbuf_.mutex);
    if (data && data->computeChainDataLength() > 0) {
      outbuf_.chunks.push_back(std::move(data));
    }
    if (fin) {
      outbuf_.finQueued = true;
    }
  }
  markActive();
  if (callback) {
    callback(true);
  }
}

compat::Expected<compat::Unit, compat::WebTransportError>
PicoquicH3StreamWriteHandle::writeStreamDataSync(
    std::unique_ptr<compat::Payload> data,
    bool fin) {
  {
    std::lock_guard<std::mutex> lock(outbuf_.mutex);
    if (data && data->computeChainDataLength() > 0) {
      outbuf_.chunks.push_back(std::move(data));
    }
    if (fin) {
      outbuf_.finQueued = true;
    }
  }
  markActive();
  return compat::unit;
}

void PicoquicH3StreamWriteHandle::resetStream(uint32_t errorCode) {
  picoquic_reset_stream(cnx_, streamId_, errorCode);
}

void PicoquicH3StreamWriteHandle::setPriority(
    const compat::StreamPriority& /*priority*/) {
  // picoquic doesn't have a priority API for streams
}

void PicoquicH3StreamWriteHandle::awaitWritable(
    std::function<void()> callback) {
  writableCb_ = std::move(callback);
}

void PicoquicH3StreamWriteHandle::setPeerCancelCallback(
    std::function<void(uint32_t)> cb) {
  cancelCb_ = std::move(cb);
}

compat::Expected<compat::Unit, compat::WebTransportError>
PicoquicH3StreamWriteHandle::registerDeliveryCallback(
    uint64_t /*offset*/,
    compat::DeliveryCallback* /*cb*/) {
  // Not implemented for h3 streams
  return compat::makeUnexpected(compat::WebTransportError::GENERIC_ERROR);
}

bool PicoquicH3StreamWriteHandle::isCancelled() const {
  return cancelled_;
}

std::optional<uint32_t> PicoquicH3StreamWriteHandle::getCancelError() const {
  return cancelError_;
}

void PicoquicH3StreamWriteHandle::onStopSending(uint32_t errorCode) {
  cancelled_ = true;
  cancelError_ = errorCode;
  if (cancelCb_) {
    cancelCb_(errorCode);
  }
}

int PicoquicH3StreamWriteHandle::onPrepareToSend(
    void* context,
    size_t maxLength) {
  std::lock_guard<std::mutex> lock(outbuf_.mutex);

  if (outbuf_.chunks.empty() && !outbuf_.finQueued) {
    return 0;  // Nothing to send
  }

  // First, calculate total data available to send
  size_t totalAvailable = 0;
  for (auto& chunk : outbuf_.chunks) {
    chunk->coalesce();
    totalAvailable += chunk->computeChainDataLength();
  }
  totalAvailable -= outbuf_.firstChunkOffset;

  size_t toSend = std::min(totalAvailable, maxLength);
  bool isFin = outbuf_.finQueued && (toSend >= totalAvailable);
  bool moreToSend = !isFin && (totalAvailable > toSend);

  if (toSend == 0 && !isFin) {
    return 0;
  }

  // Call picoquic_provide_stream_data_buffer ONCE with total size
  uint8_t* buffer = picoquic_provide_stream_data_buffer(
      context, toSend, isFin ? 1 : 0, moreToSend ? 1 : 0);

  if (!buffer && toSend > 0) {
    return 0;  // Buffer not available
  }

  // Now copy all chunks into the single buffer
  size_t offset = 0;
  while (!outbuf_.chunks.empty() && offset < toSend) {
    auto& chunk = outbuf_.chunks.front();
    size_t chunkLen = chunk->computeChainDataLength();
    size_t available = chunkLen - outbuf_.firstChunkOffset;
    size_t toCopy = std::min(available, toSend - offset);

    const uint8_t* chunkData = chunk->data();
    std::memcpy(buffer + offset, chunkData + outbuf_.firstChunkOffset, toCopy);
    offset += toCopy;
    outbuf_.firstChunkOffset += toCopy;

    if (outbuf_.firstChunkOffset >= chunkLen) {
      outbuf_.chunks.pop_front();
      outbuf_.firstChunkOffset = 0;
    }
  }

  if (isFin) {
    outbuf_.finSent = true;
  }

  return 0;
}

void PicoquicH3StreamWriteHandle::markActive() {
  if (dispatcher_ && !dispatcher_->isOnPicoThread()) {
    dispatcher_->runOnPicoThread(
        [this]() { picoquic_mark_active_stream(cnx_, streamId_, 1, nullptr); });
  } else {
    picoquic_mark_active_stream(cnx_, streamId_, 1, nullptr);
  }
}

// ============================================================================
// PicoquicH3StreamReadHandle implementation
// ============================================================================

PicoquicH3StreamReadHandle::PicoquicH3StreamReadHandle(
    picoquic_cnx_t* cnx,
    uint64_t streamId,
    h3zero_callback_ctx_t* h3Ctx,
    PicoquicThreadDispatcher* dispatcher)
    : cnx_(cnx),
      streamId_(streamId),
      h3Ctx_(h3Ctx),
      dispatcher_(dispatcher) {}

uint64_t PicoquicH3StreamReadHandle::getID() const {
  return streamId_;
}

void PicoquicH3StreamReadHandle::setReadCallback(
    std::function<void(compat::StreamData, std::optional<uint32_t> error)> cb) {
  readCb_ = std::move(cb);
  deliverBuffered();
}

void PicoquicH3StreamReadHandle::pauseReading() {
  paused_ = true;
}

void PicoquicH3StreamReadHandle::resumeReading() {
  paused_ = false;
  deliverBuffered();
}

compat::Expected<compat::Unit, compat::WebTransportError>
PicoquicH3StreamReadHandle::stopSending(uint32_t error) {
  picoquic_stop_sending(cnx_, streamId_, error);
  return compat::unit;
}

bool PicoquicH3StreamReadHandle::isFinished() const {
  return finished_;
}

void PicoquicH3StreamReadHandle::onData(
    const uint8_t* data,
    size_t length,
    bool fin) {
  if (fin) {
    finished_ = true;
  }

  if (paused_ || !readCb_) {
    // Buffer the data
    BufferedData bd;
    if (length > 0) {
      bd.data = compat::Payload::copyBuffer(data, length);
    }
    bd.fin = fin;
    buffered_.push_back(std::move(bd));
    return;
  }

  compat::StreamData streamData;
  if (length > 0) {
    streamData.data = compat::Payload::copyBuffer(data, length);
  }
  streamData.fin = fin;
  readCb_(std::move(streamData), std::nullopt);
}

void PicoquicH3StreamReadHandle::onError(uint32_t errorCode) {
  if (readCb_) {
    readCb_(compat::StreamData{}, errorCode);
  }
}

void PicoquicH3StreamReadHandle::deliverBuffered() {
  if (!readCb_ || paused_) {
    return;
  }

  for (auto& bd : buffered_) {
    compat::StreamData streamData;
    streamData.data = std::move(bd.data);
    streamData.fin = bd.fin;
    readCb_(std::move(streamData), std::nullopt);
  }
  buffered_.clear();
}

// --- PicoquicH3BidiStreamHandle ---

PicoquicH3BidiStreamHandle::PicoquicH3BidiStreamHandle(
    std::unique_ptr<PicoquicH3StreamWriteHandle> write,
    std::unique_ptr<PicoquicH3StreamReadHandle> read)
    : writeHandle_(std::move(write)), readHandle_(std::move(read)) {}

} // namespace moxygen::transports

#endif // MOXYGEN_QUIC_PICOQUIC
