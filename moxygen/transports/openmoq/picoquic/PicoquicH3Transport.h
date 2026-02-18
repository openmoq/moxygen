/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <moxygen/compat/Config.h>

#if MOXYGEN_QUIC_PICOQUIC

#include <moxygen/compat/WebTransportInterface.h>
#include <moxygen/transports/openmoq/picoquic/PicoquicRawTransport.h>

// picoquic HTTP/3 headers
#include <h3zero.h>
#include <h3zero_common.h>

#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace moxygen::transports {

// Forward declarations
class PicoquicH3StreamWriteHandle;
class PicoquicH3StreamReadHandle;
class PicoquicH3BidiStreamHandle;

/**
 * PicoquicH3Transport implements WebTransportInterface using picoquic's
 * HTTP/3 (h3zero) implementation. This enables interoperability with
 * mvfst/proxygen WebTransport servers.
 *
 * Unlike PicoquicRawTransport (raw QUIC), this class:
 * - Uses ALPN "h3" for HTTP/3
 * - Performs WebTransport upgrade via HTTP CONNECT
 * - Frames MoQ streams within WebTransport sessions
 *
 * Thread safety: All picoquic API calls are dispatched to the picoquic thread
 * via PicoquicThreadDispatcher.
 */
class PicoquicH3Transport : public compat::WebTransportInterface {
 public:
  // Create client-side transport (will initiate WebTransport upgrade)
  PicoquicH3Transport(
      picoquic_cnx_t* cnx,
      const std::string& path,  // WebTransport endpoint path (e.g., "/moq")
      PicoquicThreadDispatcher* dispatcher = nullptr);

  // Create server-side transport (WebTransport session already established)
  PicoquicH3Transport(
      picoquic_cnx_t* cnx,
      h3zero_callback_ctx_t* h3Ctx,
      void* wtSessionCtx,
      PicoquicThreadDispatcher* dispatcher = nullptr);

  ~PicoquicH3Transport() override;

  // Initialize HTTP/3 context (call before starting connection)
  bool initH3Context();

  // Initiate WebTransport upgrade (client-side, call after QUIC connected)
  bool initiateWebTransportUpgrade();

  // HTTP/3 callback - called by picoquic for HTTP/3 events
  static int h3Callback(
      picoquic_cnx_t* cnx,
      uint64_t stream_id,
      uint8_t* bytes,
      size_t length,
      picohttp_call_back_event_t event,
      h3zero_stream_ctx_t* stream_ctx,
      void* callback_ctx);

  // WebTransport callback - called for WebTransport session events
  static int wtCallback(
      picoquic_cnx_t* cnx,
      uint8_t* bytes,
      size_t length,
      picohttp_call_back_event_t event,
      h3zero_stream_ctx_t* stream_ctx,
      void* callback_ctx);

  // QUIC callback - called for raw QUIC events
  static int quicCallback(
      picoquic_cnx_t* cnx,
      uint64_t stream_id,
      uint8_t* bytes,
      size_t length,
      picoquic_call_back_event_t event,
      void* callback_ctx,
      void* stream_ctx);

  // --- WebTransportInterface ---

  compat::Expected<compat::StreamWriteHandle*, compat::WebTransportError>
  createUniStream() override;

  compat::Expected<compat::BidiStreamHandle*, compat::WebTransportError>
  createBidiStream() override;

  compat::Expected<compat::Unit, compat::WebTransportError> sendDatagram(
      std::unique_ptr<compat::Payload> data) override;

  void setMaxDatagramSize(size_t maxSize) override;
  [[nodiscard]] size_t getMaxDatagramSize() const override;

  void closeSession(uint32_t errorCode = 0) override;
  void drainSession() override;

  [[nodiscard]] compat::SocketAddress getPeerAddress() const override;
  [[nodiscard]] compat::SocketAddress getLocalAddress() const override;
  [[nodiscard]] std::string getALPN() const override;
  [[nodiscard]] bool isConnected() const override;

  void setNewUniStreamCallback(
      std::function<void(compat::StreamReadHandle*)> cb) override;
  void setNewBidiStreamCallback(
      std::function<void(compat::BidiStreamHandle*)> cb) override;
  void setDatagramCallback(
      std::function<void(std::unique_ptr<compat::Payload>)> cb) override;
  void setSessionCloseCallback(
      std::function<void(std::optional<uint32_t> error)> cb) override;
  void setSessionDrainCallback(std::function<void()> cb) override;

  // Check if WebTransport upgrade has completed
  [[nodiscard]] bool isWebTransportReady() const {
    return wtReady_;
  }

  // Set callback for when WebTransport upgrade completes
  void setWebTransportReadyCallback(std::function<void(bool success)> cb) {
    wtReadyCb_ = std::move(cb);
  }

  // Set callback for when QUIC connection is ready
  void setConnectionReadyCallback(std::function<void()> cb) {
    connectionReadyCb_ = std::move(cb);
  }

  // Set callback for connection close
  void setConnectionCloseCallback(std::function<void(uint32_t)> cb) {
    connectionCloseCb_ = std::move(cb);
  }

  // Access underlying connection
  picoquic_cnx_t* getConnection() const {
    return cnx_;
  }

  h3zero_callback_ctx_t* getH3Context() const {
    return h3Ctx_;
  }

  PicoquicThreadDispatcher* getDispatcher() const {
    return dispatcher_;
  }

 public:
  // Handle HTTP/3 events (public for server-side callback access)
  int onH3Event(
      uint64_t streamId,
      uint8_t* bytes,
      size_t length,
      picohttp_call_back_event_t event,
      h3zero_stream_ctx_t* streamCtx);

 private:

  // Handle WebTransport upgrade response
  void onConnectAccepted();
  void onConnectRefused();

  // Handle incoming WebTransport stream
  void onNewWtStream(uint64_t streamId, bool isUnidirectional);

  // Handle incoming datagram
  void onDatagram(const uint8_t* bytes, size_t length);

  // Handle provide datagram callback (JIT send)
  int onProvideDatagram(uint8_t* context, size_t maxLength);

  picoquic_cnx_t* cnx_;
  std::string path_;
  bool isServer_;
  PicoquicThreadDispatcher* dispatcher_;

  // HTTP/3 context (owned if client, borrowed if server)
  h3zero_callback_ctx_t* h3Ctx_{nullptr};
  bool ownsH3Ctx_{false};

  // WebTransport session context
  void* wtSessionCtx_{nullptr};
  uint64_t controlStreamId_{UINT64_MAX};
  h3zero_stream_ctx_t* controlStreamCtx_{nullptr};
  std::string authority_;

  // Stream tracking
  std::unordered_map<uint64_t, std::unique_ptr<PicoquicH3StreamWriteHandle>>
      writeStreams_;
  std::unordered_map<uint64_t, std::unique_ptr<PicoquicH3StreamReadHandle>>
      readStreams_;
  std::unordered_map<uint64_t, std::unique_ptr<PicoquicH3BidiStreamHandle>>
      bidiStreams_;

  // Callbacks
  std::function<void(compat::StreamReadHandle*)> newUniStreamCb_;
  std::function<void(compat::BidiStreamHandle*)> newBidiStreamCb_;
  std::function<void(std::unique_ptr<compat::Payload>)> datagramCb_;
  std::function<void(std::optional<uint32_t>)> sessionCloseCb_;
  std::function<void()> sessionDrainCb_;
  std::function<void(bool)> wtReadyCb_;
  std::function<void()> connectionReadyCb_;
  std::function<void(uint32_t)> connectionCloseCb_;

  // State
  bool closed_{false};
  bool wtReady_{false};
  size_t maxDatagramSize_{1200};

  // Datagram queue for sendDatagram
  std::mutex datagramMutex_;
  std::deque<std::unique_ptr<compat::Payload>> datagramQueue_;
};

/**
 * Stream write handle for HTTP/3 WebTransport streams.
 */
class PicoquicH3StreamWriteHandle : public compat::StreamWriteHandle {
 public:
  PicoquicH3StreamWriteHandle(
      picoquic_cnx_t* cnx,
      uint64_t streamId,
      h3zero_callback_ctx_t* h3Ctx,
      PicoquicThreadDispatcher* dispatcher = nullptr);
  ~PicoquicH3StreamWriteHandle() override = default;

  [[nodiscard]] uint64_t getID() const override;

  void writeStreamData(
      std::unique_ptr<compat::Payload> data,
      bool fin,
      std::function<void(bool success)> callback) override;

  compat::Expected<compat::Unit, compat::WebTransportError> writeStreamDataSync(
      std::unique_ptr<compat::Payload> data,
      bool fin) override;

  void resetStream(uint32_t errorCode) override;
  void setPriority(const compat::StreamPriority& priority) override;
  void awaitWritable(std::function<void()> callback) override;
  void setPeerCancelCallback(std::function<void(uint32_t)> cb) override;

  compat::Expected<compat::Unit, compat::WebTransportError>
  registerDeliveryCallback(uint64_t offset, compat::DeliveryCallback* cb)
      override;

  [[nodiscard]] bool isCancelled() const override;
  [[nodiscard]] std::optional<uint32_t> getCancelError() const override;

  // Called from h3zero callback for JIT send
  int onPrepareToSend(void* context, size_t maxLength);

  // Called when peer sends STOP_SENDING
  void onStopSending(uint32_t errorCode);

 private:
  void markActive();

  picoquic_cnx_t* cnx_;
  uint64_t streamId_;
  h3zero_callback_ctx_t* h3Ctx_;
  PicoquicThreadDispatcher* dispatcher_;

  bool cancelled_{false};
  std::optional<uint32_t> cancelError_;
  std::function<void(uint32_t)> cancelCb_;
  std::function<void()> writableCb_;

  // Buffer for JIT send
  struct OutBuffer {
    std::mutex mutex;
    std::deque<std::unique_ptr<compat::Payload>> chunks;
    size_t firstChunkOffset{0};
    bool finQueued{false};
    bool finSent{false};
  };
  OutBuffer outbuf_;
};

/**
 * Stream read handle for HTTP/3 WebTransport streams.
 */
class PicoquicH3StreamReadHandle : public compat::StreamReadHandle {
 public:
  PicoquicH3StreamReadHandle(
      picoquic_cnx_t* cnx,
      uint64_t streamId,
      h3zero_callback_ctx_t* h3Ctx,
      PicoquicThreadDispatcher* dispatcher = nullptr);
  ~PicoquicH3StreamReadHandle() override = default;

  [[nodiscard]] uint64_t getID() const override;

  void setReadCallback(
      std::function<void(compat::StreamData, std::optional<uint32_t> error)> cb)
      override;

  void pauseReading() override;
  void resumeReading() override;

  compat::Expected<compat::Unit, compat::WebTransportError> stopSending(
      uint32_t error) override;

  [[nodiscard]] bool isFinished() const override;

  // Called when data arrives
  void onData(const uint8_t* data, size_t length, bool fin);
  void onError(uint32_t errorCode);

 private:
  void deliverBuffered();

  picoquic_cnx_t* cnx_;
  uint64_t streamId_;
  h3zero_callback_ctx_t* h3Ctx_;
  PicoquicThreadDispatcher* dispatcher_;

  bool finished_{false};
  bool paused_{false};
  std::function<void(compat::StreamData, std::optional<uint32_t>)> readCb_;

  struct BufferedData {
    std::unique_ptr<compat::Payload> data;
    bool fin{false};
  };
  std::vector<BufferedData> buffered_;
};

/**
 * Bidirectional stream handle for HTTP/3 WebTransport streams.
 */
class PicoquicH3BidiStreamHandle : public compat::BidiStreamHandle {
 public:
  PicoquicH3BidiStreamHandle(
      std::unique_ptr<PicoquicH3StreamWriteHandle> write,
      std::unique_ptr<PicoquicH3StreamReadHandle> read);
  ~PicoquicH3BidiStreamHandle() override = default;

  compat::StreamWriteHandle* writeHandle() override {
    return writeHandle_.get();
  }
  compat::StreamReadHandle* readHandle() override {
    return readHandle_.get();
  }

 private:
  std::unique_ptr<PicoquicH3StreamWriteHandle> writeHandle_;
  std::unique_ptr<PicoquicH3StreamReadHandle> readHandle_;
};

} // namespace moxygen::transports

#endif // MOXYGEN_QUIC_PICOQUIC
