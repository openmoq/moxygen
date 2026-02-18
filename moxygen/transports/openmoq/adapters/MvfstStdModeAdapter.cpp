/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <moxygen/transports/openmoq/adapters/MvfstStdModeAdapter.h>

#if MOXYGEN_QUIC_MVFST

#include <folly/io/async/EventBase.h>
#include <folly/io/async/ScopedEventBaseThread.h>
#include <folly/futures/Promise.h>
#include <folly/MaybeManagedPtr.h>
#include <proxygen/lib/http/webtransport/QuicWebTransport.h>
#include <quic/client/QuicClientTransport.h>
#include <quic/fizz/client/handshake/FizzClientQuicHandshakeContext.h>
#include <fizz/client/FizzClientContext.h>
#include <fizz/protocol/CertificateVerifier.h>

#include <moxygen/compat/Payload.h>
#include <moxygen/util/QuicConnector.h>
#include <quic/priority/HTTPPriorityQueue.h>

namespace moxygen::transports {

// Implementation of StreamWriteHandle for mvfst
class MvfstStdModeAdapter::StreamWriteHandleImpl
    : public compat::StreamWriteHandle {
 public:
  StreamWriteHandleImpl(
      proxygen::WebTransport::StreamWriteHandle* handle,
      MvfstStdModeAdapter* adapter)
      : handle_(handle), adapter_(adapter) {}

  [[nodiscard]] uint64_t getID() const override {
    return handle_->getID();
  }

  void writeStreamData(
      std::unique_ptr<compat::Payload> data,
      bool fin,
      std::function<void(bool success)> callback) override {
    // Convert to shared_ptr to allow copying in std::function
    auto sharedData = std::shared_ptr<compat::Payload>(std::move(data));
    adapter_->runOnEventBase([this, sharedData, fin,
                              cb = std::move(callback)]() mutable {
      std::unique_ptr<folly::IOBuf> buf;
      if (sharedData && !sharedData->empty()) {
        buf = folly::IOBuf::copyBuffer(sharedData->data(), sharedData->length());
      }
      auto result = handle_->writeStreamData(std::move(buf), fin, nullptr);
      if (cb) {
        bool success = !result.hasError();
        adapter_->invokeCallback([cb = std::move(cb), success]() {
          cb(success);
        });
      }
    });
  }

  compat::Expected<compat::Unit, compat::WebTransportError> writeStreamDataSync(
      std::unique_ptr<compat::Payload> data,
      bool fin) override {
    // Convert to shared_ptr to allow copying in std::function
    auto sharedData = std::shared_ptr<compat::Payload>(std::move(data));
    return adapter_->runOnEventBaseSync<
        compat::Expected<compat::Unit, compat::WebTransportError>>(
        [this, sharedData, fin]() mutable {
          std::unique_ptr<folly::IOBuf> buf;
          if (sharedData && !sharedData->empty()) {
            buf = folly::IOBuf::copyBuffer(sharedData->data(), sharedData->length());
          }
          auto result = handle_->writeStreamData(std::move(buf), fin, nullptr);
          if (result.hasError()) {
            return compat::Expected<compat::Unit, compat::WebTransportError>(
                compat::makeUnexpected(compat::WebTransportError::SEND_ERROR));
          }
          return compat::Expected<compat::Unit, compat::WebTransportError>(
              compat::Unit{});
        });
  }

  void resetStream(uint32_t errorCode) override {
    adapter_->runOnEventBase([this, errorCode]() {
      handle_->resetStream(errorCode);
    });
  }

  void setPriority(const compat::StreamPriority& priority) override {
    adapter_->runOnEventBase([this, priority]() {
      quic::HTTPPriorityQueue::Priority p(
          priority.urgency, priority.incremental, priority.order);
      handle_->setPriority(p);
    });
  }

  void awaitWritable(std::function<void()> callback) override {
    adapter_->runOnEventBase([this, cb = std::move(callback)]() mutable {
      auto result = handle_->awaitWritable();
      if (result.hasError()) {
        return;
      }
      std::move(result.value())
          .via(adapter_->evb_)
          .thenValue([this, cb = std::move(cb)](uint64_t) mutable {
            adapter_->invokeCallback(std::move(cb));
          });
    });
  }

  void setPeerCancelCallback(std::function<void(uint32_t)> cb) override {
    cancelCb_ = std::move(cb);
  }

  compat::Expected<compat::Unit, compat::WebTransportError>
  registerDeliveryCallback(
      uint64_t /*offset*/,
      compat::DeliveryCallback* /*cb*/) override {
    // Delivery callbacks require more complex integration
    return compat::makeUnexpected(compat::WebTransportError::GENERIC_ERROR);
  }

  [[nodiscard]] bool isCancelled() const override {
    return cancelError_.has_value();
  }

  [[nodiscard]] std::optional<uint32_t> getCancelError() const override {
    return cancelError_;
  }

  void setCancelError(uint32_t error) {
    cancelError_ = error;
    if (cancelCb_) {
      adapter_->invokeCallback([cb = cancelCb_, error]() {
        cb(error);
      });
    }
  }

 private:
  proxygen::WebTransport::StreamWriteHandle* handle_;
  MvfstStdModeAdapter* adapter_;
  std::function<void(uint32_t)> cancelCb_;
  std::optional<uint32_t> cancelError_;
};

// Implementation of StreamReadHandle for mvfst
class MvfstStdModeAdapter::StreamReadHandleImpl
    : public compat::StreamReadHandle {
 public:
  StreamReadHandleImpl(
      proxygen::WebTransport::StreamReadHandle* handle,
      MvfstStdModeAdapter* adapter)
      : handle_(handle), adapter_(adapter) {}

  [[nodiscard]] uint64_t getID() const override {
    return handle_->getID();
  }

  void setReadCallback(
      std::function<void(compat::StreamData, std::optional<uint32_t> error)>
          cb) override {
    adapter_->runOnEventBase([this, cb = std::move(cb)]() mutable {
      readCb_ = cb;
      // Use awaitNextRead with callback API
      handle_->awaitNextRead(
          adapter_->evb_,
          [this](proxygen::WebTransport::StreamReadHandle*,
                 uint64_t,
                 folly::Try<proxygen::WebTransport::StreamData> result) {
            if (result.hasException()) {
              finished_ = true;
              if (readCb_) {
                // Try to extract error code from exception
                std::optional<uint32_t> errorCode;
                try {
                  result.exception().throw_exception();
                } catch (const proxygen::WebTransport::Exception& ex) {
                  errorCode = ex.error;
                } catch (...) {
                  errorCode = 0; // Generic error
                }
                adapter_->invokeCallback(
                    [cb = readCb_, errorCode]() {
                      cb(compat::StreamData{}, errorCode);
                    });
              }
              return;
            }

            auto& data = result.value();
            bool fin = data.fin;
            if (fin) {
              finished_ = true;
            }

            std::shared_ptr<compat::Payload> payload;
            if (data.data) {
              // Use copyBuffer factory method
              auto p = compat::Payload::copyBuffer(
                  data.data->data(), data.data->length());
              payload = std::shared_ptr<compat::Payload>(std::move(p));
            }

            if (readCb_) {
              adapter_->invokeCallback(
                  [cb = readCb_, payload, fin]() mutable {
                    compat::StreamData sd;
                    if (payload) {
                      sd.data = payload->clone();
                    }
                    sd.fin = fin;
                    cb(std::move(sd), std::nullopt);
                  });
            }

            // Continue reading if not finished and callback still set
            if (!finished_ && readCb_) {
              setReadCallback(std::move(readCb_));
            }
          });
    });
  }

  void pauseReading() override {
    adapter_->runOnEventBase([this]() {
      readCb_ = nullptr;
    });
  }

  void resumeReading() override {
    // Reading is resumed by setting callback again
  }

  compat::Expected<compat::Unit, compat::WebTransportError> stopSending(
      uint32_t error) override {
    adapter_->runOnEventBase([this, error]() {
      handle_->stopSending(error);
    });
    return compat::Unit{};
  }

  [[nodiscard]] bool isFinished() const override {
    return finished_;
  }

 private:
  proxygen::WebTransport::StreamReadHandle* handle_;
  MvfstStdModeAdapter* adapter_;
  std::function<void(compat::StreamData, std::optional<uint32_t>)> readCb_;
  bool finished_{false};
};

// Implementation of BidiStreamHandle for mvfst
class MvfstStdModeAdapter::BidiStreamHandleImpl : public compat::BidiStreamHandle {
 public:
  BidiStreamHandleImpl(
      std::unique_ptr<StreamWriteHandleImpl> write,
      std::unique_ptr<StreamReadHandleImpl> read)
      : writeHandle_(std::move(write)), readHandle_(std::move(read)) {}

  compat::StreamWriteHandle* writeHandle() override {
    return writeHandle_.get();
  }

  compat::StreamReadHandle* readHandle() override {
    return readHandle_.get();
  }

 private:
  std::unique_ptr<StreamWriteHandleImpl> writeHandle_;
  std::unique_ptr<StreamReadHandleImpl> readHandle_;
};

// MvfstStdModeAdapter implementation

MvfstStdModeAdapter::MvfstStdModeAdapter(Config config)
    : config_(std::move(config)) {
  evbThread_ = std::make_unique<folly::ScopedEventBaseThread>("MvfstEvb");
  evb_ = evbThread_->getEventBase();
}

MvfstStdModeAdapter::~MvfstStdModeAdapter() {
  shutdown_ = true;

  // Close session on EventBase thread
  if (evb_ && wt_) {
    evb_->runInEventBaseThreadAndWait([this]() {
      if (wt_) {
        wt_->closeSession(folly::none);
        wt_.reset();
      }
    });
  }

  // EventBase thread will be joined when evbThread_ is destroyed
}

std::unique_ptr<MvfstStdModeAdapter> MvfstStdModeAdapter::create(Config config) {
  return std::unique_ptr<MvfstStdModeAdapter>(
      new MvfstStdModeAdapter(std::move(config)));
}

void MvfstStdModeAdapter::connect(std::function<void(bool success)> callback) {
  evb_->runInEventBaseThread([this, cb = std::move(callback)]() mutable {
    // Create certificate verifier
    std::shared_ptr<fizz::CertificateVerifier> verifier;
    if (!config_.verifyPeer) {
      verifier = std::make_shared<fizz::DefaultCertificateVerifier>(
          fizz::VerificationContext::Client);
    }

    // Resolve address
    folly::SocketAddress addr(config_.host, config_.port, true);

    // ALPN for MoQ
    std::vector<std::string> alpns = {"moq-00"};

    quic::TransportSettings transportSettings;
    transportSettings.datagramConfig.enabled = true;

    // Use coroutine to connect
    folly::coro::co_invoke(
        [this, addr, verifier = std::move(verifier), alpns,
         transportSettings]() mutable -> folly::coro::Task<void> {
          auto quicClient = co_await QuicConnector::connectQuic(
              evb_,
              addr,
              std::chrono::milliseconds(config_.connectTimeoutMs),
              std::move(verifier),
              alpns,
              transportSettings);

          // Create QuicWebTransport
          wt_ = std::make_shared<proxygen::QuicWebTransport>(std::move(quicClient));
          connected_ = true;
        })
        .scheduleOn(evb_)
        .start([this, cb = std::move(cb)](folly::Try<void>&& result) mutable {
          bool success = !result.hasException();
          if (success) {
            connected_ = true;
          }
          invokeCallback([cb = std::move(cb), success]() {
            cb(success);
          });
        });
  });
}

void MvfstStdModeAdapter::runOnEventBase(std::function<void()> fn) {
  if (evb_->isInEventBaseThread()) {
    fn();
  } else {
    evb_->runInEventBaseThread(std::move(fn));
  }
}

template <typename T>
T MvfstStdModeAdapter::runOnEventBaseSync(std::function<T()> fn) {
  if (evb_->isInEventBaseThread()) {
    return fn();
  }

  folly::Promise<T> promise;
  auto future = promise.getFuture();

  evb_->runInEventBaseThread([fn = std::move(fn),
                              promise = std::move(promise)]() mutable {
    promise.setValue(fn());
  });

  return std::move(future).get();
}

void MvfstStdModeAdapter::invokeCallback(std::function<void()> callback) {
  if (config_.callbackExecutor) {
    config_.callbackExecutor(std::move(callback));
  } else {
    // Invoke on EventBase thread if no executor specified
    callback();
  }
}

compat::Expected<compat::StreamWriteHandle*, compat::WebTransportError>
MvfstStdModeAdapter::createUniStream() {
  return runOnEventBaseSync<
      compat::Expected<compat::StreamWriteHandle*, compat::WebTransportError>>(
      [this]() -> compat::Expected<
                   compat::StreamWriteHandle*,
                   compat::WebTransportError> {
        if (!wt_ || !connected_) {
          return compat::makeUnexpected(
              compat::WebTransportError::SESSION_TERMINATED);
        }

        auto result = wt_->createUniStream();
        if (result.hasError()) {
          return compat::makeUnexpected(
              compat::WebTransportError::STREAM_CREATION_ERROR);
        }

        auto* handle = result.value();
        auto id = handle->getID();
        auto impl = std::make_unique<StreamWriteHandleImpl>(handle, this);
        auto* ptr = impl.get();
        writeHandles_[id] = std::move(impl);
        return ptr;
      });
}

compat::Expected<compat::BidiStreamHandle*, compat::WebTransportError>
MvfstStdModeAdapter::createBidiStream() {
  return runOnEventBaseSync<
      compat::Expected<compat::BidiStreamHandle*, compat::WebTransportError>>(
      [this]() -> compat::Expected<
                   compat::BidiStreamHandle*,
                   compat::WebTransportError> {
        if (!wt_ || !connected_) {
          return compat::makeUnexpected(
              compat::WebTransportError::SESSION_TERMINATED);
        }

        auto result = wt_->createBidiStream();
        if (result.hasError()) {
          return compat::makeUnexpected(
              compat::WebTransportError::STREAM_CREATION_ERROR);
        }

        auto bidiHandle = result.value();
        if (!bidiHandle.writeHandle) {
          return compat::makeUnexpected(
              compat::WebTransportError::STREAM_CREATION_ERROR);
        }

        auto id = bidiHandle.writeHandle->getID();
        auto writeImpl =
            std::make_unique<StreamWriteHandleImpl>(bidiHandle.writeHandle, this);
        auto readImpl =
            std::make_unique<StreamReadHandleImpl>(bidiHandle.readHandle, this);
        auto impl = std::make_unique<BidiStreamHandleImpl>(
            std::move(writeImpl), std::move(readImpl));
        auto* ptr = impl.get();
        bidiHandles_[id] = std::move(impl);
        return ptr;
      });
}

compat::Expected<compat::Unit, compat::WebTransportError>
MvfstStdModeAdapter::sendDatagram(std::unique_ptr<compat::Payload> data) {
  // Convert to shared_ptr to allow copying in std::function
  auto sharedData = std::shared_ptr<compat::Payload>(std::move(data));
  return runOnEventBaseSync<
      compat::Expected<compat::Unit, compat::WebTransportError>>(
      [this, sharedData]() mutable {
        if (!wt_ || !connected_) {
          return compat::Expected<compat::Unit, compat::WebTransportError>(
              compat::makeUnexpected(
                  compat::WebTransportError::SESSION_TERMINATED));
        }

        std::unique_ptr<folly::IOBuf> buf;
        if (sharedData && !sharedData->empty()) {
          buf = folly::IOBuf::copyBuffer(sharedData->data(), sharedData->length());
        }

        auto result = wt_->sendDatagram(std::move(buf));
        if (result.hasError()) {
          return compat::Expected<compat::Unit, compat::WebTransportError>(
              compat::makeUnexpected(compat::WebTransportError::SEND_ERROR));
        }
        return compat::Expected<compat::Unit, compat::WebTransportError>(
            compat::Unit{});
      });
}

void MvfstStdModeAdapter::setMaxDatagramSize(size_t /*maxSize*/) {
  // proxygen::WebTransport doesn't expose this
}

size_t MvfstStdModeAdapter::getMaxDatagramSize() const {
  return 1200; // Reasonable default
}

void MvfstStdModeAdapter::closeSession(uint32_t errorCode) {
  runOnEventBase([this, errorCode]() {
    if (wt_) {
      wt_->closeSession(errorCode > 0 ? folly::Optional<uint32_t>(errorCode)
                                      : folly::none);
      connected_ = false;
    }
  });
}

void MvfstStdModeAdapter::drainSession() {
  // proxygen::WebTransport doesn't expose drain directly
  // Close with no error is similar
  closeSession(0);
}

compat::SocketAddress MvfstStdModeAdapter::getPeerAddress() const {
  // Return configured address
  return compat::SocketAddress(config_.host, config_.port);
}

compat::SocketAddress MvfstStdModeAdapter::getLocalAddress() const {
  return compat::SocketAddress("0.0.0.0", 0);
}

std::string MvfstStdModeAdapter::getALPN() const {
  return "moq-00";
}

bool MvfstStdModeAdapter::isConnected() const {
  return connected_;
}

void MvfstStdModeAdapter::setNewUniStreamCallback(
    std::function<void(compat::StreamReadHandle*)> cb) {
  std::lock_guard<std::mutex> lock(callbackMutex_);
  newUniStreamCb_ = std::move(cb);
}

void MvfstStdModeAdapter::setNewBidiStreamCallback(
    std::function<void(compat::BidiStreamHandle*)> cb) {
  std::lock_guard<std::mutex> lock(callbackMutex_);
  newBidiStreamCb_ = std::move(cb);
}

void MvfstStdModeAdapter::setDatagramCallback(
    std::function<void(std::unique_ptr<compat::Payload>)> cb) {
  std::lock_guard<std::mutex> lock(callbackMutex_);
  datagramCb_ = std::move(cb);
}

void MvfstStdModeAdapter::setSessionCloseCallback(
    std::function<void(std::optional<uint32_t> error)> cb) {
  std::lock_guard<std::mutex> lock(callbackMutex_);
  sessionCloseCb_ = std::move(cb);
}

void MvfstStdModeAdapter::setSessionDrainCallback(std::function<void()> cb) {
  std::lock_guard<std::mutex> lock(callbackMutex_);
  sessionDrainCb_ = std::move(cb);
}

// MvfstStdModeServer implementation

class MvfstStdModeServer::Impl {
 public:
  explicit Impl(Config config) : config_(std::move(config)) {
    evbThread_ = std::make_unique<folly::ScopedEventBaseThread>("MvfstServerEvb");
    evb_ = evbThread_->getEventBase();
  }

  ~Impl() {
    stop();
  }

  void start(std::function<void(bool success)> callback) {
    // Server implementation would go here
    // For now, just indicate not implemented
    if (callback) {
      callback(false);
    }
  }

  void stop() {
    running_ = false;
  }

  uint16_t getPort() const {
    return config_.port;
  }

 private:
  Config config_;
  std::unique_ptr<folly::ScopedEventBaseThread> evbThread_;
  folly::EventBase* evb_{nullptr};
  std::atomic<bool> running_{false};
};

MvfstStdModeServer::MvfstStdModeServer(Config config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

MvfstStdModeServer::~MvfstStdModeServer() = default;

std::unique_ptr<MvfstStdModeServer> MvfstStdModeServer::create(Config config) {
  return std::unique_ptr<MvfstStdModeServer>(
      new MvfstStdModeServer(std::move(config)));
}

void MvfstStdModeServer::start(std::function<void(bool success)> callback) {
  impl_->start(std::move(callback));
}

void MvfstStdModeServer::stop() {
  impl_->stop();
}

uint16_t MvfstStdModeServer::getPort() const {
  return impl_->getPort();
}

} // namespace moxygen::transports

#endif // MOXYGEN_QUIC_MVFST
