/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

// Mock transport for MoQ session testing
// Works in all build modes (std-mode and Folly modes)

#include <moxygen/MoQCodec.h>
#include <moxygen/MoQFramer.h>
#include <moxygen/MoQTypes.h>
#include <moxygen/compat/BufferIO.h>

#include <gmock/gmock.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <vector>

namespace moxygen::test {

/**
 * MockStreamHandle - represents a unidirectional or bidirectional stream
 */
class MockStreamHandle {
 public:
  explicit MockStreamHandle(uint64_t streamId, bool isBidi = false)
      : streamId_(streamId), isBidi_(isBidi) {}

  uint64_t getStreamId() const {
    return streamId_;
  }

  bool isBidirectional() const {
    return isBidi_;
  }

  // Write data to the stream
  void write(std::unique_ptr<Buffer> data) {
    std::lock_guard<std::mutex> lock(mutex_);
    writeQueue_.append(std::move(data));
  }

  // Read data from the stream
  std::unique_ptr<Buffer> read() {
    std::lock_guard<std::mutex> lock(mutex_);
    return writeQueue_.move();
  }

  // Check if there's data to read
  bool hasData() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !writeQueue_.empty();
  }

  // Close the stream
  void close() {
    closed_ = true;
  }

  bool isClosed() const {
    return closed_;
  }

  // Set FIN flag
  void setFin() {
    fin_ = true;
  }

  bool hasFin() const {
    return fin_;
  }

 private:
  uint64_t streamId_;
  bool isBidi_;
  mutable std::mutex mutex_;
  BufQueue writeQueue_{BufQueue::cacheChainLength()};
  std::atomic<bool> closed_{false};
  std::atomic<bool> fin_{false};
};

/**
 * MockMoQTransport - simulates a WebTransport-like connection for testing
 *
 * This provides a simplified interface for testing MoQ sessions without
 * requiring a real QUIC transport. It can be used in all build modes.
 */
class MockMoQTransport {
 public:
  MockMoQTransport() = default;
  virtual ~MockMoQTransport() = default;

  // Stream creation
  MOCK_METHOD(
      std::shared_ptr<MockStreamHandle>,
      createUnidirectionalStream,
      ());
  MOCK_METHOD(
      std::shared_ptr<MockStreamHandle>,
      createBidirectionalStream,
      ());

  // Datagram support
  MOCK_METHOD(void, sendDatagram, (std::unique_ptr<Buffer> data));
  MOCK_METHOD(bool, supportsDatagrams, (), (const));

  // Connection state
  MOCK_METHOD(void, close, ());
  MOCK_METHOD(void, closeWithError, (uint32_t errorCode, const std::string& reason));
  MOCK_METHOD(bool, isConnected, (), (const));

  // Control stream (bidirectional)
  void setControlStream(std::shared_ptr<MockStreamHandle> stream) {
    controlStream_ = std::move(stream);
  }

  std::shared_ptr<MockStreamHandle> getControlStream() const {
    return controlStream_;
  }

  // Simulate receiving data on control stream
  void simulateControlData(std::unique_ptr<Buffer> data) {
    if (controlStream_) {
      std::lock_guard<std::mutex> lock(incomingMutex_);
      incomingControlData_.append(std::move(data));
    }
  }

  // Get incoming control data
  std::unique_ptr<Buffer> getIncomingControlData() {
    std::lock_guard<std::mutex> lock(incomingMutex_);
    return incomingControlData_.move();
  }

  // Simulate an incoming unidirectional stream
  void simulateIncomingUniStream(std::shared_ptr<MockStreamHandle> stream) {
    std::lock_guard<std::mutex> lock(streamsMutex_);
    incomingUniStreams_.push(std::move(stream));
  }

  // Get next incoming unidirectional stream
  std::shared_ptr<MockStreamHandle> getNextIncomingUniStream() {
    std::lock_guard<std::mutex> lock(streamsMutex_);
    if (incomingUniStreams_.empty()) {
      return nullptr;
    }
    auto stream = std::move(incomingUniStreams_.front());
    incomingUniStreams_.pop();
    return stream;
  }

  // Track outgoing streams
  void trackOutgoingStream(std::shared_ptr<MockStreamHandle> stream) {
    std::lock_guard<std::mutex> lock(streamsMutex_);
    outgoingStreams_.push_back(std::move(stream));
  }

  const std::vector<std::shared_ptr<MockStreamHandle>>& getOutgoingStreams()
      const {
    return outgoingStreams_;
  }

  // Link two transports for bidirectional communication
  static void link(MockMoQTransport& client, MockMoQTransport& server) {
    client.peer_ = &server;
    server.peer_ = &client;
  }

  MockMoQTransport* getPeer() const {
    return peer_;
  }

 protected:
  std::shared_ptr<MockStreamHandle> controlStream_;
  std::mutex incomingMutex_;
  BufQueue incomingControlData_{BufQueue::cacheChainLength()};

  std::mutex streamsMutex_;
  std::queue<std::shared_ptr<MockStreamHandle>> incomingUniStreams_;
  std::vector<std::shared_ptr<MockStreamHandle>> outgoingStreams_;

  MockMoQTransport* peer_{nullptr};
  uint64_t nextStreamId_{0};
};

/**
 * Helper to create a connected pair of mock transports
 */
inline std::pair<
    std::unique_ptr<MockMoQTransport>,
    std::unique_ptr<MockMoQTransport>>
createLinkedTransports() {
  auto client = std::make_unique<MockMoQTransport>();
  auto server = std::make_unique<MockMoQTransport>();
  MockMoQTransport::link(*client, *server);

  // Set up control streams
  auto clientControlStream = std::make_shared<MockStreamHandle>(0, true);
  auto serverControlStream = std::make_shared<MockStreamHandle>(1, true);
  client->setControlStream(clientControlStream);
  server->setControlStream(serverControlStream);

  return {std::move(client), std::move(server)};
}

} // namespace moxygen::test
