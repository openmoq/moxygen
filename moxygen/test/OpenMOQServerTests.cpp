/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

// MoQServer tests for openmoq-only behavior, kept out of upstream's test files
// so a sync merge cannot drop them.

#include <fizz/client/FizzClientContext.h>
#include <folly/ScopeGuard.h>
#include <folly/Singleton.h>
#include <folly/coro/BlockingWait.h>
#include <folly/coro/Promise.h>
#include <folly/coro/Task.h>
#include <folly/coro/Timeout.h>
#include <folly/io/async/ScopedEventBaseThread.h>
#include <folly/portability/GTest.h>
#include <proxygen/httpserver/samples/hq/FizzContext.h>
#include <proxygen/lib/http/HQConnector.h>
#include <proxygen/lib/http/HeaderConstants.h>
#include <proxygen/lib/http/session/HQUpstreamSession.h>
#include <proxygen/lib/http/webtransport/HTTPWebTransport.h>
#include <moxygen/MoQServer.h>
#include <moxygen/util/InsecureVerifierDangerousDoNotUseInProduction.h>

using namespace std::chrono_literals;

namespace moxygen::test {
namespace {

const std::string kEndpoint = "/test";

class H3ConnectCallback : public proxygen::HQConnector::Callback {
 public:
  void connectSuccess(proxygen::HQUpstreamSession* session) override {
    session_.first.setValue(session);
  }
  void connectError(const quic::QuicErrorCode& code) override {
    session_.first.setException(std::runtime_error(quic::toString(code)));
  }
  folly::coro::Future<proxygen::HQUpstreamSession*> session() {
    return std::move(session_.second);
  }

 private:
  std::pair<
      folly::coro::Promise<proxygen::HQUpstreamSession*>,
      folly::coro::Future<proxygen::HQUpstreamSession*>>
      session_{
          folly::coro::makePromiseContract<proxygen::HQUpstreamSession*>()};
};

// Must outlive the transaction; tests drop the connection before destroying it.
class ConnectResponseHandler : public proxygen::HTTPTransactionHandler {
 public:
  void setTransaction(proxygen::HTTPTransaction*) noexcept override {}
  void detachTransaction() noexcept override {}
  void onHeadersComplete(
      std::unique_ptr<proxygen::HTTPMessage> resp) noexcept override {
    status_.first.setValue(resp->getStatusCode());
  }
  void onBody(std::unique_ptr<folly::IOBuf>) noexcept override {}
  void onTrailers(std::unique_ptr<proxygen::HTTPHeaders>) noexcept override {}
  void onEOM() noexcept override {}
  void onUpgrade(proxygen::UpgradeProtocol) noexcept override {}
  void onError(const proxygen::HTTPException& ex) noexcept override {
    if (!status_.first.isFulfilled()) {
      status_.first.setException(std::runtime_error(ex.what()));
    }
  }
  void onEgressPaused() noexcept override {}
  void onEgressResumed() noexcept override {}

  folly::coro::Future<uint16_t> status() {
    return std::move(status_.second);
  }

 private:
  std::pair<folly::coro::Promise<uint16_t>, folly::coro::Future<uint16_t>>
      status_{folly::coro::makePromiseContract<uint16_t>()};
};

// Offers only h3, so the server has no MoQT protocol for WebTransport.
class NoMoqtProtocolServer : public MoQServer {
 public:
  NoMoqtProtocolServer()
      : MoQServer(
            quic::samples::createFizzServerContextWithInsecureDefault(
                {"h3"}, fizz::server::ClientAuthMode::None, "", ""),
            kEndpoint) {}

  void onNewSession(std::shared_ptr<MoQSession>) override {}
};

class OpenMOQServerTest : public ::testing::Test {
 public:
  void SetUp() override {
    folly::SingletonVault::singleton()->registrationComplete();
    server_ = std::make_shared<NoMoqtProtocolServer>();
    server_->start(folly::SocketAddress("::", 0));
    server_->waitUntilInitialized();
    auto fds = server_->getAllListeningSocketFDs();
    ASSERT_FALSE(fds.empty());
    folly::SocketAddress bound;
    bound.setFromLocalAddress(folly::NetworkSocket::fromFd(fds[0]));
    serverPort_ = bound.getPort();
  }

  void TearDown() override {
    server_->stop();
    server_.reset();
  }

 protected:
  // Sends a WebTransport CONNECT on a fresh connection and returns the status.
  uint16_t connectStatus(const std::vector<std::string>& wtProtocols) {
    return folly::coro::blockingWait(
        folly::coro::co_withExecutor(
            clientEvbThread_.getEventBase(),
            folly::coro::timeout(sendConnect(wtProtocols), 10s)));
  }

 private:
  folly::coro::Task<uint16_t> sendConnect(
      std::vector<std::string> wtProtocols) {
    H3ConnectCallback callback;
    proxygen::HQConnector connector(&callback, 5s);
    connector.setSupportedQuicVersions({quic::QuicVersion::QUIC_V1});
    connector.setH3Settings(
        {{proxygen::SettingsId::ENABLE_CONNECT_PROTOCOL, 1},
         {proxygen::SettingsId::_HQ_DATAGRAM, 1},
         {proxygen::SettingsId::_HQ_DATAGRAM_RFC, 1},
         {proxygen::SettingsId::ENABLE_WEBTRANSPORT, 1}});
    auto fizzContext = std::make_shared<fizz::client::FizzClientContext>();
    fizzContext->setSupportedAlpns({"h3"});
    connector.connect(
        clientEvbThread_.getEventBase(),
        folly::none,
        folly::SocketAddress("localhost", serverPort_, true),
        std::move(fizzContext),
        std::make_shared<InsecureVerifierDangerousDoNotUseInProduction>(),
        5s,
        folly::emptySocketOptionMap,
        std::string("localhost"));
    auto* session = co_await callback.session();
    ConnectResponseHandler handler;
    SCOPE_EXIT {
      session->dropConnection();
    };

    proxygen::HTTPMessage req;
    req.setHTTPVersion(1, 1);
    req.setSecure(true);
    req.getHeaders().set(proxygen::HTTP_HEADER_HOST, "localhost");
    req.getHeaders().add(
        proxygen::headers::kSecWebTransportHttp3Draft02,
        proxygen::headers::kSecWebTransportHttp3Draft02Value);
    req.setURL(kEndpoint);
    req.setMethod(proxygen::HTTPMethod::CONNECT);
    req.setUpgradeProtocol(std::string{proxygen::headers::kWebTransport});
    if (!wtProtocols.empty()) {
      proxygen::HTTPWebTransport::setWTAvailableProtocols(req, wtProtocols);
    }
    auto* txn = session->newTransaction(&handler);
    if (!txn) {
      co_yield folly::coro::co_error(
          std::runtime_error("Failed to open CONNECT transaction"));
    }
    txn->sendHeaders(req);
    co_return co_await handler.status();
  }

  std::shared_ptr<NoMoqtProtocolServer> server_;
  uint16_t serverPort_{0};
  folly::ScopedEventBaseThread clientEvbThread_{"OpenMOQServerTestClient"};
};

} // namespace

// Without a MoQT protocol to offer, the server cannot negotiate one, and
// in-band SETUP is only for a server that offers draft 14.
TEST_F(OpenMOQServerTest, NoProtocolsRejectsConnectWithoutProtocolHeader) {
  EXPECT_EQ(connectStatus({}), 400);
}

TEST_F(OpenMOQServerTest, NoProtocolsRejectsConnectWithProtocols) {
  EXPECT_EQ(connectStatus({"moqt-16"}), 400);
}

} // namespace moxygen::test
