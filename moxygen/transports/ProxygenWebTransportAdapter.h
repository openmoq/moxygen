/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <moxygen/compat/Config.h>

// This adapter is only available when using mvfst backend (which uses proxygen)
#if MOXYGEN_QUIC_MVFST

#include <moxygen/compat/WebTransportInterface.h>

#include <folly/MaybeManagedPtr.h>
#include <proxygen/lib/http/webtransport/WebTransport.h>

#include <memory>
#include <unordered_map>

namespace moxygen::transports {

// Forward declarations
class ProxygenStreamWriteHandle;
class ProxygenStreamReadHandle;
class ProxygenBidiStreamHandle;

/**
 * ProxygenWebTransportAdapter wraps proxygen::WebTransport and implements
 * compat::WebTransportInterface. This allows moxygen to use a common
 * WebTransport interface regardless of the underlying QUIC implementation.
 */
class ProxygenWebTransportAdapter : public compat::WebTransportInterface {
 public:
  explicit ProxygenWebTransportAdapter(
      folly::MaybeManagedPtr<proxygen::WebTransport> wt);
  ~ProxygenWebTransportAdapter() override;

  // Get the underlying proxygen::WebTransport (for migration purposes)
  proxygen::WebTransport* getProxygenWebTransport() const {
    return wt_.get();
  }

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

 private:
  folly::MaybeManagedPtr<proxygen::WebTransport> wt_;

  // Owned stream handle wrappers (keyed by stream ID)
  std::unordered_map<uint64_t, std::unique_ptr<ProxygenStreamWriteHandle>>
      writeHandles_;
  std::unordered_map<uint64_t, std::unique_ptr<ProxygenStreamReadHandle>>
      readHandles_;
  std::unordered_map<uint64_t, std::unique_ptr<ProxygenBidiStreamHandle>>
      bidiHandles_;

  // Callbacks
  std::function<void(compat::StreamReadHandle*)> newUniStreamCb_;
  std::function<void(compat::BidiStreamHandle*)> newBidiStreamCb_;
  std::function<void(std::unique_ptr<compat::Payload>)> datagramCb_;
  std::function<void(std::optional<uint32_t>)> sessionCloseCb_;
  std::function<void()> sessionDrainCb_;
};

/**
 * Wrapper for proxygen::WebTransport::StreamWriteHandle
 */
class ProxygenStreamWriteHandle : public compat::StreamWriteHandle {
 public:
  explicit ProxygenStreamWriteHandle(
      proxygen::WebTransport::StreamWriteHandle* handle);
  ~ProxygenStreamWriteHandle() override = default;

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

  // Access underlying handle
  proxygen::WebTransport::StreamWriteHandle* getProxygenHandle() const {
    return handle_;
  }

 private:
  proxygen::WebTransport::StreamWriteHandle* handle_;
  std::function<void(uint32_t)> cancelCb_;
  std::optional<uint32_t> cancelError_;
};

/**
 * Wrapper for proxygen::WebTransport::StreamReadHandle
 */
class ProxygenStreamReadHandle : public compat::StreamReadHandle {
 public:
  explicit ProxygenStreamReadHandle(
      proxygen::WebTransport::StreamReadHandle* handle);
  ~ProxygenStreamReadHandle() override = default;

  [[nodiscard]] uint64_t getID() const override;

  void setReadCallback(
      std::function<void(compat::StreamData, std::optional<uint32_t> error)> cb)
      override;

  void pauseReading() override;
  void resumeReading() override;

  compat::Expected<compat::Unit, compat::WebTransportError> stopSending(
      uint32_t error) override;

  [[nodiscard]] bool isFinished() const override;

  // Access underlying handle
  proxygen::WebTransport::StreamReadHandle* getProxygenHandle() const {
    return handle_;
  }

 private:
  proxygen::WebTransport::StreamReadHandle* handle_;
  bool finished_{false};
};

/**
 * Wrapper for proxygen bidirectional stream
 */
class ProxygenBidiStreamHandle : public compat::BidiStreamHandle {
 public:
  ProxygenBidiStreamHandle(
      std::unique_ptr<ProxygenStreamWriteHandle> write,
      std::unique_ptr<ProxygenStreamReadHandle> read);
  ~ProxygenBidiStreamHandle() override = default;

  compat::StreamWriteHandle* writeHandle() override {
    return writeHandle_.get();
  }
  compat::StreamReadHandle* readHandle() override {
    return readHandle_.get();
  }

 private:
  std::unique_ptr<ProxygenStreamWriteHandle> writeHandle_;
  std::unique_ptr<ProxygenStreamReadHandle> readHandle_;
};

} // namespace moxygen::transports

#endif // MOXYGEN_QUIC_MVFST
