/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "moxygen/openmoq/transport/pico/MoQPicoQuicShardedServer.h"
#include <folly/ScopeGuard.h>
#include <folly/Try.h>
#include <folly/logging/xlog.h>
#include <moxygen/openmoq/transport/pico/MoQPicoQuicEventBaseServer.h>
#include <stdexcept>

namespace moxygen {

// Forwards the overridable virtuals to the parent so a subclass's overrides
// run unchanged. validateAuthority() needs none: MoQSession reaches it through
// the ServerSetupCallback& that parent_->createSession() bound to *parent_.
class MoQPicoQuicShardedServer::ShardServer
    : public MoQPicoQuicEventBaseServer {
 public:
  ShardServer(
      MoQPicoQuicShardedServer* parent,
      std::string cert,
      std::string key,
      std::string endpoint,
      folly::Executor::KeepAlive<folly::EventBase> evb,
      std::string versions,
      PicoTransportConfig transportConfig,
      PicoWebTransportConfig wtConfig,
      bool reusePort)
      : MoQPicoQuicEventBaseServer(
            std::move(cert),
            std::move(key),
            std::move(endpoint),
            std::move(evb),
            std::move(versions),
            std::move(transportConfig),
            std::move(wtConfig),
            reusePort),
        parent_(parent) {}

  void onNewSession(std::shared_ptr<MoQSession> session) override {
    parent_->onNewSession(std::move(session));
  }

  void terminateClientSession(std::shared_ptr<MoQSession> session) override {
    parent_->terminateClientSession(std::move(session));
  }

 protected:
  std::shared_ptr<MoQSession> createSession(
      folly::MaybeManagedPtr<proxygen::WebTransport> wt,
      std::shared_ptr<MoQExecutor> executor) override {
    return parent_->createSession(std::move(wt), std::move(executor));
  }

  // handleClientSession sends the proactive SERVER_SETUP from the shard.
  Setup makeServerSetup() override {
    return parent_->makeServerSetup();
  }

 private:
  MoQPicoQuicShardedServer* parent_;
};

MoQPicoQuicShardedServer::MoQPicoQuicShardedServer(
    std::string cert,
    std::string key,
    std::string endpoint,
    std::string versions,
    PicoTransportConfig transportConfig,
    PicoWebTransportConfig wtConfig)
    : MoQServerBase(endpoint),
      cert_(std::move(cert)),
      key_(std::move(key)),
      endpoint_(std::move(endpoint)),
      versions_(std::move(versions)),
      transportConfig_(std::move(transportConfig)),
      wtConfig_(std::move(wtConfig)) {}

MoQPicoQuicShardedServer::~MoQPicoQuicShardedServer() {
  if (started_ && !stopped_) {
    stop();
  }
}

void MoQPicoQuicShardedServer::start(
    const folly::SocketAddress& addr,
    std::vector<folly::EventBase*> evbs) {
  XCHECK(!stopped_) << "MoQPicoQuicShardedServer::start called after stop()";
  XCHECK(!started_) << "MoQPicoQuicShardedServer::start called twice";

  auto rollback = folly::makeGuard([this] { teardown(); });

  if (evbs.empty()) {
    ownedWorkers_.push_back(
        std::make_unique<folly::ScopedEventBaseThread>("MoQPicoShard"));
    evbs.push_back(ownedWorkers_.back()->getEventBase());
  } else {
    for (auto* evb : evbs) {
      XCHECK(evb) << "null EventBase in MoQPicoQuicShardedServer worker pool";
    }
  }

  const bool sharded = evbs.size() > 1;
  PicoTransportConfig cfg = transportConfig_;
  if (sharded && !cfg.disableMigration) {
    XLOG(WARN) << "Sharding picoquic across " << evbs.size()
               << " threads; forcing disableMigration=true (SO_REUSEPORT "
                  "cannot route a migrated connection's packets to the "
                  "shard holding its state)";
    cfg.disableMigration = true;
  }

  // On port 0 the first shard's bind picks an ephemeral port; the rest bind
  // to that port to join the same reuseport group.
  folly::SocketAddress bindAddr = addr;

  shards_.reserve(evbs.size());
  for (auto* workerEvb : evbs) {
    auto destroyed = std::make_shared<folly::Baton<>>();
    // The deleter runs after every base destructor, so teardown()'s waiter
    // cannot outrace ~MoQPicoServerBase.
    shards_.push_back(Shard{
        workerEvb,
        std::shared_ptr<ShardServer>(
            new ShardServer(
                this,
                cert_,
                key_,
                endpoint_,
                folly::getKeepAliveToken(workerEvb),
                versions_,
                cfg,
                wtConfig_,
                sharded),
            [destroyed](ShardServer* shard) {
              delete shard;
              destroyed->post();
            }),
        destroyed});
    auto& shard = shards_.back().server;
    if (mLoggerFactory_) {
      shard->setMLoggerFactory(mLoggerFactory_);
    }

    folly::Try<void> bound;
    workerEvb->runImmediatelyOrRunInEventBaseThreadAndWait([&] {
      bound = folly::makeTryWith([&] {
        if (statsFactory_) {
          shard->setPicoQuicStatsCallback(statsFactory_(workerEvb));
        }
        shard->start(bindAddr);
      });
    });
    // runImmediatelyOrRunInEventBaseThreadAndWait is noexcept, so a bind
    // failure has to cross back over the thread boundary by hand.
    bound.throwUnlessValue();
    auto shardAddr = shard->getAddress();
    if (!shardAddr.isInitialized()) {
      throw std::runtime_error(
          "MoQPicoQuicShardedServer: shard did not bind; check cert and key");
    }
    if (bindAddr.getPort() == 0) {
      bindAddr = shardAddr;
    }
  }

  boundAddr_ = bindAddr;
  started_ = true;
  rollback.dismiss();
  XLOG(DBG1) << "MoQPicoQuicShardedServer listening on "
             << boundAddr_.describe() << " across " << shards_.size()
             << " shard(s)" << (sharded ? " (SO_REUSEPORT)" : "");
}

void MoQPicoQuicShardedServer::stop() {
  XCHECK(!isInWorkerPool())
      << "MoQPicoQuicShardedServer::stop must not be called from a shard's "
         "EventBase thread";
  if (!started_ || stopped_) {
    return;
  }
  stopped_ = true;
  teardown();
}

void MoQPicoQuicShardedServer::teardown() {
  for (auto& shard : shards_) {
    // Drop our reference on the shard's own EventBase thread:
    // ~MoQPicoServerBase frees statsCallback_ under a still-running loop.
    shard.evb->runImmediatelyOrRunInEventBaseThreadAndWait([&] {
      shard.server->stop();
      shard.server.reset();
    });
    // A draining session holds a reference of its own. Wait it out, because
    // the shard's callbacks forward to this parent.
    shard.destroyed->wait();
  }
  shards_.clear();
  ownedWorkers_.clear();
}

} // namespace moxygen
