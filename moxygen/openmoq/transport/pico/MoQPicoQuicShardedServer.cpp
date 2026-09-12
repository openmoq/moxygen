/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "moxygen/openmoq/transport/pico/MoQPicoQuicShardedServer.h"
#include <folly/ScopeGuard.h>
#include <folly/logging/xlog.h>
#include <moxygen/openmoq/transport/pico/MoQPicoQuicEventBaseServer.h>

namespace moxygen {

// Forwards the picoquic-callback-side virtuals to the parent
// MoQPicoQuicShardedServer (or whatever further subclasses it), so a
// subclass's createSession()/onNewSession()/terminateClientSession()
// overrides run unmodified regardless of how many shards exist.
//
// validateAuthority() needs no forwarding override here: MoQSession invokes
// it through the ServerSetupCallback& bound at session-construction time
// (see MoQServerBase::createSession), which parent_->createSession()
// already binds to *parent_, not to this shard.
class MoQPicoQuicShardedServer::ShardServer : public MoQPicoQuicEventBaseServer {
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
  stop();
}

void MoQPicoQuicShardedServer::start(
    const folly::SocketAddress& addr,
    std::vector<folly::EventBase*> evbs) {
  CHECK(!stopped_) << "MoQPicoQuicShardedServer::start called after stop()";
  CHECK(!started_) << "MoQPicoQuicShardedServer::start called twice";

  auto rollback = folly::makeGuard([this] { stop(); });

  if (evbs.empty()) {
    ownedWorkers_.push_back(
        std::make_unique<folly::ScopedEventBaseThread>("MoQPicoShard"));
    workerEvbs_.push_back(ownedWorkers_.back()->getEventBase());
  } else {
    for (auto* evb : evbs) {
      CHECK(evb) << "null EventBase in MoQPicoQuicShardedServer worker pool";
    }
    workerEvbs_ = std::move(evbs);
  }

  const bool sharded = workerEvbs_.size() > 1;
  PicoTransportConfig cfg = transportConfig_;
  if (sharded && !cfg.disableMigration) {
    XLOG(WARN) << "Sharding picoquic across " << workerEvbs_.size()
               << " threads; forcing disableMigration=true (SO_REUSEPORT "
                  "cannot route a migrated connection's packets to the "
                  "shard holding its state)";
  }
  if (sharded) {
    cfg.disableMigration = true;
  }

  // If addr has port 0, the first shard's bind picks an ephemeral port; the
  // rest bind to that concrete port to join the same reuseport group.
  folly::SocketAddress bindAddr = addr;

  shards_.reserve(workerEvbs_.size());
  for (auto* workerEvb : workerEvbs_) {
    auto shard = std::make_unique<ShardServer>(
        this,
        cert_,
        key_,
        endpoint_,
        folly::getKeepAliveToken(workerEvb),
        versions_,
        cfg,
        wtConfig_,
        sharded);
    if (mLoggerFactory_) {
      shard->setMLoggerFactory(mLoggerFactory_);
    }
    if (statsFactory_) {
      shard->setPicoQuicStatsCallback(statsFactory_(workerEvb));
    }
    workerEvb->runImmediatelyOrRunInEventBaseThreadAndWait(
        [&] { shard->start(bindAddr); });
    if (bindAddr.getPort() == 0) {
      bindAddr = shard->getBoundAddress();
    }
    shards_.push_back(std::move(shard));
  }

  boundAddr_ = bindAddr;
  started_ = true;
  rollback.dismiss();
  XLOG(DBG1) << "MoQPicoQuicShardedServer listening on "
             << boundAddr_.describe() << " across " << shards_.size()
             << " shard(s)" << (sharded ? " (SO_REUSEPORT)" : "");
}

void MoQPicoQuicShardedServer::stop() {
  if (stopped_) {
    return;
  }
  stopped_ = true;

  for (size_t i = 0; i < shards_.size(); ++i) {
    workerEvbs_[i]->runImmediatelyOrRunInEventBaseThreadAndWait(
        [&] { shards_[i]->stop(); });
  }
  shards_.clear();
  ownedWorkers_.clear();
  workerEvbs_.clear();
}

} // namespace moxygen
