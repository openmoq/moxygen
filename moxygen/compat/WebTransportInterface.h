/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <moxygen/compat/Config.h>
#include <moxygen/compat/Expected.h>
#include <moxygen/compat/Payload.h>
#include <moxygen/compat/SocketAddress.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace moxygen::compat {

// Error codes for WebTransport operations
enum class WebTransportError : uint32_t {
  GENERIC_ERROR = 0x00,
  INVALID_STREAM_ID,
  STREAM_CREATION_ERROR,
  SEND_ERROR,
  STOP_SENDING,
  SESSION_TERMINATED,
  BLOCKED
};

// Result of a stream read operation
struct StreamData {
  std::unique_ptr<Payload> data;
  bool fin{false};
};

// Abstract handle for writing to a stream
class StreamWriteHandle {
 public:
  virtual ~StreamWriteHandle() = default;

  // Get the stream ID
  [[nodiscard]] virtual uint64_t getID() const = 0;

  // Write data to the stream
  // Returns error if write fails
  virtual Expected<Unit, WebTransportError> writeStreamData(
      std::unique_ptr<Payload> data,
      bool fin) = 0;

  // Reset the stream with an error code
  virtual void resetStream(uint32_t errorCode) = 0;

  // Set callback for when stream is cancelled by peer
  virtual void setPeerCancelCallback(std::function<void(uint32_t)> cb) = 0;

  // Delivery callback support
  class DeliveryCallback {
   public:
    virtual ~DeliveryCallback() = default;
    virtual void onDelivery(uint64_t streamId, uint64_t offset) = 0;
    virtual void onCancellation(uint64_t streamId, uint64_t offset) = 0;
  };

  // Register for delivery notification at a specific offset
  virtual Expected<Unit, WebTransportError> registerDeliveryCallback(
      uint64_t offset,
      DeliveryCallback* cb) = 0;
};

// Abstract handle for reading from a stream
class StreamReadHandle {
 public:
  virtual ~StreamReadHandle() = default;

  // Get the stream ID
  [[nodiscard]] virtual uint64_t getID() const = 0;

  // Set callback for incoming data
  // Callback receives: (data, fin, error)
  // If error is set, the stream encountered an error
  virtual void setReadCallback(
      std::function<void(StreamData, std::optional<uint32_t> error)> cb) = 0;

  // Stop receiving data from peer
  virtual Expected<Unit, WebTransportError> stopSending(uint32_t error) = 0;
};

// Bidirectional stream handle combines read and write
class BidiStreamHandle {
 public:
  virtual ~BidiStreamHandle() = default;

  virtual StreamWriteHandle* writeHandle() = 0;
  virtual StreamReadHandle* readHandle() = 0;
};

// Abstract WebTransport interface
// This decouples MoQSession from proxygen::WebTransport
class WebTransportInterface {
 public:
  virtual ~WebTransportInterface() = default;

  // Create a unidirectional stream for sending
  virtual Expected<StreamWriteHandle*, WebTransportError> createUniStream() = 0;

  // Create a bidirectional stream
  virtual Expected<BidiStreamHandle*, WebTransportError> createBidiStream() = 0;

  // Send a datagram (unreliable)
  virtual Expected<Unit, WebTransportError> sendDatagram(
      std::unique_ptr<Payload> data) = 0;

  // Close the session
  virtual void closeSession(uint32_t errorCode = 0) = 0;

  // Get peer address
  virtual SocketAddress getPeerAddress() const = 0;

  // Set callback for new incoming unidirectional streams from peer
  virtual void setNewUniStreamCallback(
      std::function<void(StreamReadHandle*)> cb) = 0;

  // Set callback for new incoming bidirectional streams from peer
  virtual void setNewBidiStreamCallback(
      std::function<void(BidiStreamHandle*)> cb) = 0;

  // Set callback for incoming datagrams
  virtual void setDatagramCallback(
      std::function<void(std::unique_ptr<Payload>)> cb) = 0;

  // Set callback for session close
  virtual void setSessionCloseCallback(
      std::function<void(std::optional<uint32_t> error)> cb) = 0;
};

} // namespace moxygen::compat
