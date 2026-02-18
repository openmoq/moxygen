/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <moxygen/compat/Async.h>
#include <moxygen/compat/Config.h>
#include <moxygen/compat/Expected.h>
#include <moxygen/compat/Payload.h>
#include <moxygen/compat/SocketAddress.h>
#include <moxygen/compat/Unit.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace moxygen::compat {

// Error codes for WebTransport operations
enum class WebTransportError : uint32_t {
  NO_ERROR = 0x00,
  GENERIC_ERROR,
  INVALID_STREAM_ID,
  STREAM_CREATION_ERROR,
  SEND_ERROR,
  STOP_SENDING,
  SESSION_TERMINATED,
  BLOCKED,
  FLOW_CONTROL
};

// Result of a stream read operation
struct StreamData {
  std::unique_ptr<Payload> data;
  bool fin{false};
};

// Stream priority (compatible with HTTP/3 priority)
struct StreamPriority {
  uint8_t urgency{3};      // 0-7, lower is more urgent
  bool incremental{false}; // Can deliver incrementally
  uint8_t order{0};        // Order within urgency level

  StreamPriority() = default;
  StreamPriority(uint8_t u, bool inc, uint8_t ord = 0)
      : urgency(u), incremental(inc), order(ord) {}
};

// Delivery callback for tracking when data is ACKed
class DeliveryCallback {
 public:
  virtual ~DeliveryCallback() = default;
  virtual void onDelivery(uint64_t streamId, uint64_t offset) = 0;
  virtual void onCancellation(uint64_t streamId, uint64_t offset) = 0;
};

// Abstract handle for writing to a stream
class StreamWriteHandle {
 public:
  virtual ~StreamWriteHandle() = default;

  // Get the stream ID
  [[nodiscard]] virtual uint64_t getID() const = 0;

  // Write data to the stream
  // The callback is invoked with true on success, false on error
  // For sync implementations, callback may be invoked before return
  virtual void writeStreamData(
      std::unique_ptr<Payload> data,
      bool fin,
      std::function<void(bool success)> callback = nullptr) = 0;

  // Synchronous write - returns immediately with result
  virtual Expected<Unit, WebTransportError> writeStreamDataSync(
      std::unique_ptr<Payload> data,
      bool fin) = 0;

  // Reset the stream with an error code
  virtual void resetStream(uint32_t errorCode) = 0;

  // Set stream priority
  virtual void setPriority(const StreamPriority& priority) = 0;

  // Flow control: Set callback for when more data can be written
  // Callback is invoked when the stream becomes writable after being blocked
  virtual void awaitWritable(std::function<void()> callback) = 0;

  // Flow control: Get a future that completes when the stream is writable
  // For JIT transports, this completes when prepare_to_send is called
  // Default implementation uses awaitWritable callback
  virtual Expected<SemiFuture<Unit>, WebTransportError> awaitWritable() {
    auto promise = std::make_shared<Promise<Unit>>();
    auto future = promise->getSemiFuture();
    awaitWritable([promise]() mutable { promise->setValue(unit); });
    return std::move(future);
  }

  // Set callback for when stream is cancelled/reset by peer
  // errorCode is the STOP_SENDING or RESET_STREAM error code
  virtual void setPeerCancelCallback(std::function<void(uint32_t)> cb) = 0;

  // Register for delivery notification at a specific byte offset
  virtual Expected<Unit, WebTransportError> registerDeliveryCallback(
      uint64_t offset,
      DeliveryCallback* cb) = 0;

  // Check if stream has been cancelled (peer sent STOP_SENDING)
  [[nodiscard]] virtual bool isCancelled() const = 0;

  // Get error if stream was cancelled
  [[nodiscard]] virtual std::optional<uint32_t> getCancelError() const = 0;
};

// Abstract handle for reading from a stream
class StreamReadHandle {
 public:
  virtual ~StreamReadHandle() = default;

  // Get the stream ID
  [[nodiscard]] virtual uint64_t getID() const = 0;

  // Set callback for incoming data
  // Callback receives: (data, error)
  // If error is set, the stream encountered an error and data may be empty
  // If data.fin is true, this is the last data on the stream
  virtual void setReadCallback(
      std::function<void(StreamData, std::optional<uint32_t> error)> cb) = 0;

  // Pause reading (temporarily stop invoking read callback)
  virtual void pauseReading() = 0;

  // Resume reading
  virtual void resumeReading() = 0;

  // Stop receiving data from peer (send STOP_SENDING)
  virtual Expected<Unit, WebTransportError> stopSending(uint32_t error) = 0;

  // Check if stream has received FIN or error
  [[nodiscard]] virtual bool isFinished() const = 0;
};

// Bidirectional stream handle combines read and write
class BidiStreamHandle {
 public:
  virtual ~BidiStreamHandle() = default;

  virtual StreamWriteHandle* writeHandle() = 0;
  virtual StreamReadHandle* readHandle() = 0;
};

// Abstract WebTransport session interface
// This decouples MoQSession from proxygen::WebTransport
class WebTransportInterface {
 public:
  virtual ~WebTransportInterface() = default;

  // --- Stream Creation ---

  // Create a unidirectional stream for sending
  // Returns nullptr if stream creation failed (e.g., flow control)
  virtual Expected<StreamWriteHandle*, WebTransportError> createUniStream() = 0;

  // Create a bidirectional stream
  virtual Expected<BidiStreamHandle*, WebTransportError> createBidiStream() = 0;

  // --- Stream Credit (Flow Control) ---

  // Wait for unidirectional stream creation credit (MAX_STREAMS)
  // Returns a future that completes when a new uni stream can be created
  // For JIT transports, this may complete immediately if credit is available
  virtual Expected<SemiFuture<Unit>, WebTransportError> awaitUniStreamCredit() {
    // Default: assume stream credit is always available
    return SemiFuture<Unit>(unit);
  }

  // Wait for bidirectional stream creation credit (MAX_STREAMS)
  virtual Expected<SemiFuture<Unit>, WebTransportError> awaitBidiStreamCredit() {
    // Default: assume stream credit is always available
    return SemiFuture<Unit>(unit);
  }

  // --- Datagrams ---

  // Send a datagram (unreliable, unordered)
  virtual Expected<Unit, WebTransportError> sendDatagram(
      std::unique_ptr<Payload> data) = 0;

  // Set maximum datagram size (0 = disable datagrams)
  virtual void setMaxDatagramSize(size_t maxSize) = 0;

  // Get maximum datagram size supported
  [[nodiscard]] virtual size_t getMaxDatagramSize() const = 0;

  // --- Session Control ---

  // Close the session with optional error code and reason
  virtual void closeSession(uint32_t errorCode = 0) = 0;

  // Drain the session (stop accepting new streams, finish existing)
  virtual void drainSession() = 0;

  // --- Connection Info ---

  // Get peer address
  [[nodiscard]] virtual SocketAddress getPeerAddress() const = 0;

  // Get local address
  [[nodiscard]] virtual SocketAddress getLocalAddress() const = 0;

  // Get negotiated ALPN (e.g., "moq-00", "h3")
  [[nodiscard]] virtual std::string getALPN() const = 0;

  // Check if session is still connected
  [[nodiscard]] virtual bool isConnected() const = 0;

  // --- Event Callbacks ---

  // Set callback for new incoming unidirectional streams from peer
  virtual void setNewUniStreamCallback(
      std::function<void(StreamReadHandle*)> cb) = 0;

  // Set callback for new incoming bidirectional streams from peer
  virtual void setNewBidiStreamCallback(
      std::function<void(BidiStreamHandle*)> cb) = 0;

  // Set callback for incoming datagrams
  virtual void setDatagramCallback(
      std::function<void(std::unique_ptr<Payload>)> cb) = 0;

  // Set callback for session close/end
  // Error is set if session terminated abnormally
  virtual void setSessionCloseCallback(
      std::function<void(std::optional<uint32_t> error)> cb) = 0;

  // Set callback for session drain (peer requested drain)
  virtual void setSessionDrainCallback(std::function<void()> cb) = 0;
};

// Handler interface for WebTransport sessions (server-side)
// The server creates a handler for each incoming session
class WebTransportHandler {
 public:
  virtual ~WebTransportHandler() = default;

  // Called when a new unidirectional stream is opened by peer
  virtual void onNewUniStream(StreamReadHandle* stream) = 0;

  // Called when a new bidirectional stream is opened by peer
  virtual void onNewBidiStream(BidiStreamHandle* stream) = 0;

  // Called when a datagram is received
  virtual void onDatagram(std::unique_ptr<Payload> datagram) = 0;

  // Called when session ends
  virtual void onSessionEnd(std::optional<uint32_t> error) = 0;

  // Called when peer requests session drain
  virtual void onSessionDrain() = 0;
};

} // namespace moxygen::compat
