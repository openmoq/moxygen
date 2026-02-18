/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <moxygen/compat/Config.h>

// This implementation is only available when using picoquic backend
#if MOXYGEN_QUIC_PICOQUIC

#include <moxygen/compat/WebTransportInterface.h>

#include <picoquic.h>
#include <picoquic_packet_loop.h>
#include <picoquic_utils.h>

#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace moxygen::transports {

/**
 * PicoquicThreadDispatcher marshals work to the picoquic network thread.
 *
 * Picoquic is not thread-safe. All picoquic API calls must happen on the
 * picoquic network thread. This dispatcher provides:
 * - runOnPicoThread(fn): async dispatch (fire-and-forget)
 * - runOnPicoThreadSync<T>(fn): sync dispatch (blocks caller until complete)
 *
 * Uses picoquic_wake_up_network_thread() for efficient cross-thread signaling.
 * The work queue is drained on the pico thread when picoquic_packet_loop_wake_up
 * fires in the loop callback.
 */
class PicoquicThreadDispatcher {
 public:
  PicoquicThreadDispatcher() = default;

  // Set after picoquic_start_network_thread() returns
  void setThreadContext(picoquic_network_thread_ctx_t* ctx);

  // Called from loop callback on picoquic_packet_loop_ready
  void setPicoThreadId(std::thread::id id);

  // Check if current thread is the picoquic thread
  bool isOnPicoThread() const;

  // Async dispatch: queue work and wake picoquic thread
  void runOnPicoThread(std::function<void()> fn);

  // Sync dispatch: queue work, wake thread, block until complete
  template <typename T>
  T runOnPicoThreadSync(std::function<T()> fn) {
    if (isOnPicoThread()) {
      return fn();
    }
    auto promise = std::make_shared<std::promise<T>>();
    auto future = promise->get_future();
    runOnPicoThread([promise, fn = std::move(fn)]() {
      try {
        promise->set_value(fn());
      } catch (...) {
        promise->set_exception(std::current_exception());
      }
    });
    return future.get();
  }

  // Called from picoquic thread on picoquic_packet_loop_wake_up event
  void drainWorkQueue();

 private:
  std::mutex mutex_;
  std::deque<std::function<void()>> workQueue_;
  picoquic_network_thread_ctx_t* threadCtx_{nullptr};
  std::thread::id picoThreadId_;
};

// Forward declarations
class PicoquicStreamWriteHandle;
class PicoquicStreamReadHandle;
class PicoquicBidiStreamHandle;

/**
 * PicoquicRawTransport implements WebTransportInterface using raw QUIC streams.
 * This provides a pure C++ implementation without Folly dependencies.
 * Uses ALPN "moq-00" for direct MoQ-over-QUIC without HTTP/3 framing.
 *
 * Thread safety: All picoquic API calls are dispatched to the picoquic thread
 * via PicoquicThreadDispatcher. The JIT send API (mark_active_stream +
 * prepare_to_send + provide_stream_data_buffer) is used for stream writes.
 */
class PicoquicRawTransport : public compat::WebTransportInterface {
 public:
  // Create from an existing picoquic connection
  explicit PicoquicRawTransport(
      picoquic_cnx_t* cnx,
      bool isServer = false,
      PicoquicThreadDispatcher* dispatcher = nullptr);

  ~PicoquicRawTransport() override;

  // picoquic callback - called by picoquic event loop
  static int picoCallback(
      picoquic_cnx_t* cnx,
      uint64_t stream_id,
      uint8_t* bytes,
      size_t length,
      picoquic_call_back_event_t fin_or_event,
      void* callback_ctx,
      void* stream_ctx);

  // --- Stream Creation ---
  compat::Expected<compat::StreamWriteHandle*, compat::WebTransportError>
  createUniStream() override;

  compat::Expected<compat::BidiStreamHandle*, compat::WebTransportError>
  createBidiStream() override;

  // --- Datagrams ---
  compat::Expected<compat::Unit, compat::WebTransportError> sendDatagram(
      std::unique_ptr<compat::Payload> data) override;

  void setMaxDatagramSize(size_t maxSize) override;
  [[nodiscard]] size_t getMaxDatagramSize() const override;

  // --- Session Control ---
  void closeSession(uint32_t errorCode = 0) override;
  void drainSession() override;

  // --- Connection Info ---
  [[nodiscard]] compat::SocketAddress getPeerAddress() const override;
  [[nodiscard]] compat::SocketAddress getLocalAddress() const override;
  [[nodiscard]] std::string getALPN() const override;
  [[nodiscard]] bool isConnected() const override;

  // --- Event Callbacks ---
  void setNewUniStreamCallback(
      std::function<void(compat::StreamReadHandle*)> cb) override;
  void setNewBidiStreamCallback(
      std::function<void(compat::BidiStreamHandle*)> cb) override;
  void setDatagramCallback(
      std::function<void(std::unique_ptr<compat::Payload>)> cb) override;
  void setSessionCloseCallback(
      std::function<void(std::optional<uint32_t> error)> cb) override;
  void setSessionDrainCallback(std::function<void()> cb) override;

  // Access underlying connection
  picoquic_cnx_t* getConnection() const {
    return cnx_;
  }

  PicoquicThreadDispatcher* getDispatcher() const {
    return dispatcher_;
  }

 private:
  // Handle stream/connection events from picoquic
  int onStreamEvent(
      uint64_t streamId,
      uint8_t* bytes,
      size_t length,
      picoquic_call_back_event_t event,
      void* streamCtx);

  // Handle incoming datagram from picoquic
  void onDatagram(uint8_t* bytes, size_t length);

  // Handle JIT datagram send callback
  int onPrepareDatagram(uint8_t* context, size_t maxLength);

  picoquic_cnx_t* cnx_;
  bool isServer_;
  PicoquicThreadDispatcher* dispatcher_;

  // Stream tracking
  std::unordered_map<uint64_t, std::unique_ptr<PicoquicStreamWriteHandle>>
      writeStreams_;
  std::unordered_map<uint64_t, std::unique_ptr<PicoquicStreamReadHandle>>
      readStreams_;
  std::unordered_map<uint64_t, std::unique_ptr<PicoquicBidiStreamHandle>>
      bidiStreams_;

  // Callbacks
  std::function<void(compat::StreamReadHandle*)> newUniStreamCb_;
  std::function<void(compat::BidiStreamHandle*)> newBidiStreamCb_;
  std::function<void(std::unique_ptr<compat::Payload>)> datagramCb_;
  std::function<void(std::optional<uint32_t>)> sessionCloseCb_;
  std::function<void()> sessionDrainCb_;

  // Outgoing datagram queue (JIT send)
  std::mutex datagramMutex_;
  std::deque<std::unique_ptr<compat::Payload>> datagramQueue_;

  // Connection state
  bool closed_{false};
  size_t maxDatagramSize_{1200};
};

/**
 * Stream write handle for picoquic using JIT send API.
 *
 * Data is buffered in OutBuffer and provided to picoquic via
 * picoquic_provide_stream_data_buffer when picoquic_callback_prepare_to_send
 * fires (indicating flow credits are available).
 */
class PicoquicStreamWriteHandle : public compat::StreamWriteHandle {
 public:
  PicoquicStreamWriteHandle(
      picoquic_cnx_t* cnx,
      uint64_t streamId,
      PicoquicThreadDispatcher* dispatcher = nullptr);
  ~PicoquicStreamWriteHandle() override = default;

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

  // JIT-aware version: returns future that completes when prepare_to_send fires
  compat::Expected<compat::SemiFuture<compat::Unit>, compat::WebTransportError>
  awaitWritable() override;

  void setPeerCancelCallback(std::function<void(uint32_t)> cb) override;

  compat::Expected<compat::Unit, compat::WebTransportError>
  registerDeliveryCallback(uint64_t offset, compat::DeliveryCallback* cb)
      override;

  [[nodiscard]] bool isCancelled() const override;
  [[nodiscard]] std::optional<uint32_t> getCancelError() const override;

  // Called by PicoquicRawTransport from picoquic_callback_prepare_to_send
  // context = opaque context for picoquic_provide_stream_data_buffer
  // maxLength = max bytes that can be sent
  int onPrepareToSend(void* context, size_t maxLength);

 private:
  // Mark stream as active on the picoquic thread
  void markActive();

  picoquic_cnx_t* cnx_;
  uint64_t streamId_;
  PicoquicThreadDispatcher* dispatcher_;
  bool cancelled_{false};
  std::optional<uint32_t> cancelError_;
  std::function<void(uint32_t)> cancelCb_;
  std::function<void()> writableCb_;

  // JIT send buffer — accessed from both threads, protected by mutex
  struct OutBuffer {
    std::mutex mutex;
    std::deque<std::unique_ptr<compat::Payload>> chunks;
    size_t firstChunkOffset{0}; // bytes already consumed from front chunk
    bool finQueued{false};      // FIN has been buffered
    bool finSent{false};        // FIN has been provided to picoquic

    // Waiters for prepare_to_send signal (JIT backpressure)
    std::vector<compat::Promise<compat::Unit>> readyWaiters;
  };
  OutBuffer outbuf_;
};

/**
 * Stream read handle for picoquic
 */
class PicoquicStreamReadHandle : public compat::StreamReadHandle {
 public:
  PicoquicStreamReadHandle(
      picoquic_cnx_t* cnx,
      uint64_t streamId,
      PicoquicThreadDispatcher* dispatcher = nullptr);
  ~PicoquicStreamReadHandle() override = default;

  [[nodiscard]] uint64_t getID() const override;

  void setReadCallback(
      std::function<void(compat::StreamData, std::optional<uint32_t> error)> cb)
      override;

  void pauseReading() override;
  void resumeReading() override;

  compat::Expected<compat::Unit, compat::WebTransportError> stopSending(
      uint32_t error) override;

  [[nodiscard]] bool isFinished() const override;

  // Called by PicoquicRawTransport when data arrives
  void onData(const uint8_t* data, size_t length, bool fin);
  void onError(uint32_t errorCode);

 private:
  void deliverBuffered();

  picoquic_cnx_t* cnx_;
  uint64_t streamId_;
  PicoquicThreadDispatcher* dispatcher_;
  bool finished_{false};
  bool paused_{false};
  std::function<void(compat::StreamData, std::optional<uint32_t>)> readCb_;

  // Buffer for data that arrives before the read callback is set
  struct BufferedData {
    std::unique_ptr<compat::Payload> data;
    bool fin{false};
  };
  std::vector<BufferedData> buffered_;
};

/**
 * Bidirectional stream handle for picoquic
 */
class PicoquicBidiStreamHandle : public compat::BidiStreamHandle {
 public:
  PicoquicBidiStreamHandle(
      std::unique_ptr<PicoquicStreamWriteHandle> write,
      std::unique_ptr<PicoquicStreamReadHandle> read);
  ~PicoquicBidiStreamHandle() override = default;

  compat::StreamWriteHandle* writeHandle() override {
    return writeHandle_.get();
  }
  compat::StreamReadHandle* readHandle() override {
    return readHandle_.get();
  }

 private:
  std::unique_ptr<PicoquicStreamWriteHandle> writeHandle_;
  std::unique_ptr<PicoquicStreamReadHandle> readHandle_;
};

} // namespace moxygen::transports

#endif // MOXYGEN_QUIC_PICOQUIC
