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
#include <picoquic_utils.h>

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace moxygen::transports {

// Forward declarations
class PicoquicStreamWriteHandle;
class PicoquicStreamReadHandle;
class PicoquicBidiStreamHandle;

/**
 * PicoquicWebTransport implements WebTransportInterface using picoquic.
 * This provides a pure C++ implementation without Folly dependencies.
 */
class PicoquicWebTransport : public compat::WebTransportInterface {
 public:
  // Create from an existing picoquic connection
  explicit PicoquicWebTransport(
      picoquic_cnx_t* cnx,
      bool isServer = false);

  ~PicoquicWebTransport() override;

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

 private:
  // Handle incoming stream data from picoquic
  int onStreamData(
      uint64_t streamId,
      uint8_t* bytes,
      size_t length,
      picoquic_call_back_event_t event,
      void* streamCtx);

  // Handle datagram from picoquic
  void onDatagram(uint8_t* bytes, size_t length);

  picoquic_cnx_t* cnx_;
  bool isServer_;

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

  // Connection state
  bool closed_{false};
  size_t maxDatagramSize_{1200};
};

/**
 * Stream write handle for picoquic
 */
class PicoquicStreamWriteHandle : public compat::StreamWriteHandle {
 public:
  PicoquicStreamWriteHandle(picoquic_cnx_t* cnx, uint64_t streamId);
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
  void setPeerCancelCallback(std::function<void(uint32_t)> cb) override;

  compat::Expected<compat::Unit, compat::WebTransportError>
  registerDeliveryCallback(uint64_t offset, compat::DeliveryCallback* cb)
      override;

  [[nodiscard]] bool isCancelled() const override;
  [[nodiscard]] std::optional<uint32_t> getCancelError() const override;

 private:
  picoquic_cnx_t* cnx_;
  uint64_t streamId_;
  bool cancelled_{false};
  std::optional<uint32_t> cancelError_;
  std::function<void(uint32_t)> cancelCb_;
  std::function<void()> writableCb_;
};

/**
 * Stream read handle for picoquic
 */
class PicoquicStreamReadHandle : public compat::StreamReadHandle {
 public:
  PicoquicStreamReadHandle(picoquic_cnx_t* cnx, uint64_t streamId);
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

  // Called by PicoquicWebTransport when data arrives
  void onData(const uint8_t* data, size_t length, bool fin);
  void onError(uint32_t errorCode);

 private:
  picoquic_cnx_t* cnx_;
  uint64_t streamId_;
  bool finished_{false};
  bool paused_{false};
  std::function<void(compat::StreamData, std::optional<uint32_t>)> readCb_;
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
