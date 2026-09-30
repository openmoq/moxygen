/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "moxygen/openmoq/transport/pico/MoQPicoQuicShardedServer.h"

#include <folly/ExceptionString.h>
#include <folly/coro/BlockingWait.h>
#include <folly/coro/Timeout.h>
#include <folly/io/async/AsyncUDPSocket.h>
#include <folly/io/async/ScopedEventBaseThread.h>
#include <folly/logging/xlog.h>
#include <folly/portability/GTest.h>
#include <folly/synchronization/Baton.h>
#include <folly/testing/TestUtil.h>
#include <moxygen/MoQClient.h>
#include <moxygen/MoQSession.h>
#include <moxygen/MoQVersions.h>
#include <moxygen/ObjectReceiver.h>
#include <moxygen/Publisher.h>
#include <moxygen/events/MoQFollyExecutorImpl.h>
#include <moxygen/util/InsecureVerifierDangerousDoNotUseInProduction.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <atomic>
#include <cstdio>
#include <exception>
#include <thread>

namespace moxygen::test {

namespace {

constexpr auto kEndpoint = "/moq-test";

struct CertFiles {
  folly::test::TemporaryDirectory dir;
  std::string cert = (dir.path() / "cert.pem").string();
  std::string key = (dir.path() / "key.pem").string();
};

std::unique_ptr<CertFiles> makeSelfSignedCert() {
  auto files = std::make_unique<CertFiles>();
  EVP_PKEY* pkey = EVP_EC_gen("P-256");
  XCHECK(pkey);
  X509* x509 = X509_new();
  XCHECK(x509);
  ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
  X509_gmtime_adj(X509_getm_notBefore(x509), 0);
  X509_gmtime_adj(X509_getm_notAfter(x509), 3600);
  X509_set_pubkey(x509, pkey);
  X509_NAME* name = X509_get_subject_name(x509);
  X509_NAME_add_entry_by_txt(
      name,
      "CN",
      MBSTRING_ASC,
      reinterpret_cast<const unsigned char*>("localhost"),
      -1,
      -1,
      0);
  X509_set_issuer_name(x509, name);
  XCHECK(X509_sign(x509, pkey, EVP_sha256()) > 0);

  FILE* certFile = fopen(files->cert.c_str(), "w");
  XCHECK(certFile);
  XCHECK(PEM_write_X509(certFile, x509));
  fclose(certFile);
  FILE* keyFile = fopen(files->key.c_str(), "w");
  XCHECK(keyFile);
  XCHECK(PEM_write_PrivateKey(
      keyFile, pkey, nullptr, nullptr, 0, nullptr, nullptr));
  fclose(keyFile);

  X509_free(x509);
  EVP_PKEY_free(pkey);
  return files;
}

// The test connects before start() returns the bound port.
uint16_t pickFreeUdpPort() {
  folly::EventBase evb;
  folly::AsyncUDPSocket socket(&evb);
  socket.bind(folly::SocketAddress("::", 0));
  auto port = socket.address().getPort();
  socket.close();
  return port;
}

class TestShardedServer : public MoQPicoQuicShardedServer {
 public:
  using MoQPicoQuicShardedServer::MoQPicoQuicShardedServer;

  void onNewSession(std::shared_ptr<MoQSession> session) override {
    session->setPublishHandler(std::make_shared<Publisher>());
    sessionExecutor = session->getExecutor();
  }

  std::atomic<folly::Executor*> sessionExecutor{nullptr};
};

class NoopReceiverCallback : public ObjectReceiverCallback {
 public:
  FlowControlState
  onObject(std::optional<TrackAlias>, const ObjectHeader&, Payload) override {
    return FlowControlState::UNBLOCKED;
  }
  void onObjectStatus(std::optional<TrackAlias>, const ObjectHeader&) override {
  }
  void onEndOfStream() override {}
  void onError(ResetStreamErrorCode) override {}
  void onPublishDone(PublishDone) override {}
};

// Only the server's default Publisher replies NOT_SUPPORTED.
folly::coro::Task<bool> serverAnswersSubscribe(
    std::shared_ptr<MoQSession> session) {
  auto receiver = std::make_shared<ObjectReceiver>(
      ObjectReceiver::SUBSCRIBE, std::make_shared<NoopReceiverCallback>());
  // GCC 11 ICEs when these temporaries are built inside the co_await.
  auto request =
      SubscribeRequest::make(FullTrackName{TrackNamespace({"ns"}), "track"});
  auto subscribe = session->subscribe(std::move(request), receiver);
  auto result = co_await folly::coro::co_awaitTry(
      folly::coro::timeout(std::move(subscribe), std::chrono::seconds(3)));
  bool notSupported = result.hasValue() && result->hasError() &&
      result->error().errorCode == SubscribeErrorCode::NOT_SUPPORTED;
  co_return notSupported;
}

} // namespace

// Blocks shard 1's EventBase so start() stalls between the two binds, then
// connects clients while shard 0 is alone in the reuseport group.
TEST(MoQPicoQuicShardedServerTest, NoShardReadsUntilEveryShardIsBound) {
  constexpr size_t kClients = 8;
  auto certs = makeSelfSignedCert();
  auto port = pickFreeUdpPort();

  folly::ScopedEventBaseThread shard0("shard0");
  folly::ScopedEventBaseThread shard1("shard1");
  folly::ScopedEventBaseThread clientThread("clients");
  auto* clientEvb = clientThread.getEventBase();

  folly::Baton<> releaseShard1;
  shard1.getEventBase()->runInEventBaseThread(
      [&releaseShard1] { releaseShard1.wait(); });

  auto server =
      std::make_shared<TestShardedServer>(certs->cert, certs->key, kEndpoint);
  folly::Baton<> shard0Binding;
  server->setPicoQuicStatsCallbackFactory(
      [&](folly::EventBase* evb) -> std::shared_ptr<PicoQuicStatsCallback> {
        if (evb == shard0.getEventBase()) {
          shard0Binding.post();
        }
        return nullptr;
      });

  // An exception escaping the thread would terminate the whole test binary.
  std::exception_ptr startError;
  std::thread starter([&] {
    try {
      server->start(
          folly::SocketAddress("::", port),
          {shard0.getEventBase(), shard1.getEventBase()});
    } catch (...) {
      startError = std::current_exception();
    }
  });

  // The factory runs on shard 0 just before its bind, so this returns after it.
  shard0Binding.wait();
  shard0.getEventBase()->runInEventBaseThreadAndWait([] {});

  // setupMoQSession holds these by reference until the handshake completes.
  const quic::TransportSettings transportSettings;
  const auto alpns = getMoqtProtocols("16", /*useStandard=*/true);
  std::vector<std::unique_ptr<MoQClient>> clients;
  std::vector<folly::SemiFuture<folly::Unit>> setups;
  clientEvb->runInEventBaseThreadAndWait([&] {
    auto exec = std::make_shared<MoQFollyExecutorImpl>(clientEvb);
    for (size_t i = 0; i < kClients; ++i) {
      clients.push_back(
          std::make_unique<MoQClient>(
              exec,
              proxygen::URL(
                  fmt::format("moqt://localhost:{}{}", port, kEndpoint)),
              std::make_shared<
                  InsecureVerifierDangerousDoNotUseInProduction>()));
      setups.push_back(
          folly::coro::co_withExecutor(
              clientEvb,
              clients.back()->setupMoQSession(
                  std::chrono::seconds(10),
                  std::chrono::seconds(3),
                  nullptr,
                  nullptr,
                  transportSettings,
                  alpns))
              .start());
    }
  });

  // A loopback handshake finishes well inside this if shard 0 is reading.
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  size_t setupsWhileHalfBound = 0;
  for (auto& setup : setups) {
    if (setup.isReady()) {
      ++setupsWhileHalfBound;
    }
  }
  EXPECT_EQ(setupsWhileHalfBound, 0);

  releaseShard1.post();
  starter.join();
  if (startError) {
    ADD_FAILURE() << "start() failed: " << folly::exceptionStr(startError);
    clientEvb->runInEventBaseThreadAndWait([&] { clients.clear(); });
    server->stop();
    return;
  }

  size_t answered = 0;
  for (size_t i = 0; i < kClients; ++i) {
    auto setup = std::move(setups[i]).getTry(std::chrono::seconds(15));
    ASSERT_TRUE(setup.hasValue()) << setup.exception().what();
    if (folly::coro::blockingWait(
            folly::coro::co_withExecutor(
                clientEvb, serverAnswersSubscribe(clients[i]->moqSession_)))) {
      ++answered;
    }
  }
  EXPECT_EQ(answered, kClients);

  clientEvb->runInEventBaseThreadAndWait([&] { clients.clear(); });
  server->stop();
}

// Session owners keep raw pointers to a session's executor and post through
// them while tearing down after stop(), until they destroy the server.
TEST(MoQPicoQuicShardedServerTest, SessionExecutorOutlivesStop) {
  auto certs = makeSelfSignedCert();
  auto port = pickFreeUdpPort();
  folly::ScopedEventBaseThread clientThread("client");
  auto* clientEvb = clientThread.getEventBase();

  auto server =
      std::make_shared<TestShardedServer>(certs->cert, certs->key, kEndpoint);
  // No EventBases, so the shard runs on a worker thread that the server owns.
  server->start(folly::SocketAddress("::", port));

  const quic::TransportSettings transportSettings;
  const auto alpns = getMoqtProtocols("16", /*useStandard=*/true);
  std::unique_ptr<MoQClient> client;
  folly::SemiFuture<folly::Unit> setup = folly::makeSemiFuture();
  clientEvb->runInEventBaseThreadAndWait([&] {
    client = std::make_unique<MoQClient>(
        std::make_shared<MoQFollyExecutorImpl>(clientEvb),
        proxygen::URL(fmt::format("moqt://localhost:{}{}", port, kEndpoint)),
        std::make_shared<InsecureVerifierDangerousDoNotUseInProduction>());
    setup = folly::coro::co_withExecutor(
                clientEvb,
                client->setupMoQSession(
                    std::chrono::seconds(10),
                    std::chrono::seconds(3),
                    nullptr,
                    nullptr,
                    transportSettings,
                    alpns))
                .start();
  });
  std::move(setup).get(std::chrono::seconds(10));
  auto* sessionExecutor = server->sessionExecutor.load();
  ASSERT_NE(sessionExecutor, nullptr);

  clientEvb->runInEventBaseThreadAndWait([&] { client.reset(); });
  server->stop();

  auto ran = std::make_shared<folly::Baton<>>();
  sessionExecutor->add([ran] { ran->post(); });
  EXPECT_TRUE(ran->try_wait_for(std::chrono::seconds(5)));
  server.reset();
}

} // namespace moxygen::test
