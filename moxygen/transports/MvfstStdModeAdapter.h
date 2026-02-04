/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <moxygen/compat/Config.h>

// This adapter is for std-mode builds that want to use mvfst as the QUIC
// backend. It wraps mvfst/proxygen and provides a callback-based interface.
// It requires linking to Folly/mvfst but doesn't expose Folly types in the API.
#if MOXYGEN_QUIC_MVFST

#include <moxygen/compat/WebTransportInterface.h>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>

// Forward declarations (avoid Folly headers in public API)
namespace folly {
class EventBase;
class ScopedEventBaseThread;
} // namespace folly

namespace proxygen {
class WebTransport;
}

namespace moxygen::transports {

/**
 * MvfstStdModeAdapter provides a callback-based WebTransportInterface
 * for std-mode builds using mvfst as the QUIC backend.
 *
 * Architecture:
 * - Owns a Folly EventBase thread internally
 * - All mvfst operations run on the EventBase thread
 * - Callbacks are delivered on a user-specified callback thread or inline
 *
 * This allows std-mode moxygen code (using MoQSessionCompat with callbacks)
 * to use mvfst for transport without exposing Folly types at the API boundary.
 *
 * Usage:
 *   // In std-mode code:
 *   auto adapter = MvfstStdModeAdapter::create(config);
 *   adapter->connect("localhost", 4433, [](bool success) {
 *     if (success) {
 *       // Use adapter as WebTransportInterface
 *     }
 *   });
 */
class MvfstStdModeAdapter : public compat::WebTransportInterface {
 public:
  struct Config {
    std::string host;
    uint16_t port{4433};
    std::string path{"/"};
    std::string certPath;
    std::string keyPath;
    std::string caCertPath;
    bool verifyPeer{true};
    uint32_t connectTimeoutMs{5000};

    // If provided, callbacks will be invoked on this thread's queue
    // If nullptr, callbacks are invoked on the EventBase thread
    std::function<void(std::function<void()>)> callbackExecutor;
  };

  // Factory method - creates and returns the adapter
  static std::unique_ptr<MvfstStdModeAdapter> create(Config config);

  ~MvfstStdModeAdapter() override;

  // Non-copyable, non-movable
  MvfstStdModeAdapter(const MvfstStdModeAdapter&) = delete;
  MvfstStdModeAdapter& operator=(const MvfstStdModeAdapter&) = delete;
  MvfstStdModeAdapter(MvfstStdModeAdapter&&) = delete;
  MvfstStdModeAdapter& operator=(MvfstStdModeAdapter&&) = delete;

  // Connect to the server (async)
  void connect(std::function<void(bool success)> callback);

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

 private:
  explicit MvfstStdModeAdapter(Config config);

  // Run a function on the EventBase thread
  void runOnEventBase(std::function<void()> fn);

  // Run a function on the EventBase thread and wait for result
  template <typename T>
  T runOnEventBaseSync(std::function<T()> fn);

  // Invoke callback on the appropriate thread
  void invokeCallback(std::function<void()> callback);

  Config config_;
  std::unique_ptr<folly::ScopedEventBaseThread> evbThread_;
  folly::EventBase* evb_{nullptr}; // Owned by evbThread_

  // The underlying proxygen WebTransport (created on EventBase thread)
  std::shared_ptr<proxygen::WebTransport> wt_;

  // Connection state
  std::atomic<bool> connected_{false};
  std::atomic<bool> shutdown_{false};

  // Callbacks (protected by mutex for thread-safe registration)
  mutable std::mutex callbackMutex_;
  std::function<void(compat::StreamReadHandle*)> newUniStreamCb_;
  std::function<void(compat::BidiStreamHandle*)> newBidiStreamCb_;
  std::function<void(std::unique_ptr<compat::Payload>)> datagramCb_;
  std::function<void(std::optional<uint32_t>)> sessionCloseCb_;
  std::function<void()> sessionDrainCb_;

  // Stream handles (keyed by stream ID, accessed on EventBase thread)
  class StreamWriteHandleImpl;
  class StreamReadHandleImpl;
  class BidiStreamHandleImpl;
  std::unordered_map<uint64_t, std::unique_ptr<StreamWriteHandleImpl>>
      writeHandles_;
  std::unordered_map<uint64_t, std::unique_ptr<StreamReadHandleImpl>>
      readHandles_;
  std::unordered_map<uint64_t, std::unique_ptr<BidiStreamHandleImpl>>
      bidiHandles_;
};

/**
 * Server-side adapter for accepting WebTransport sessions via mvfst
 */
class MvfstStdModeServer {
 public:
  struct Config {
    std::string host{"0.0.0.0"};
    uint16_t port{4433};
    std::string certPath;
    std::string keyPath;

    // Called when a new session is accepted
    // The callback receives the WebTransportInterface for the session
    std::function<void(std::unique_ptr<compat::WebTransportInterface>)>
        onNewSession;

    // Executor for invoking callbacks (optional)
    std::function<void(std::function<void()>)> callbackExecutor;
  };

  static std::unique_ptr<MvfstStdModeServer> create(Config config);

  ~MvfstStdModeServer();

  // Start listening for connections (async)
  void start(std::function<void(bool success)> callback);

  // Stop the server
  void stop();

  // Get the port the server is listening on
  [[nodiscard]] uint16_t getPort() const;

 private:
  explicit MvfstStdModeServer(Config config);

  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace moxygen::transports

#endif // MOXYGEN_QUIC_MVFST
