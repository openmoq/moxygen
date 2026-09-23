/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/Function.h>
#include <folly/SocketAddress.h>
#include <folly/io/async/EventBase.h>
#include <folly/io/async/ScopedEventBaseThread.h>
#include <folly/synchronization/Baton.h>
#include <moxygen/MoQServerBase.h>
#include <moxygen/openmoq/transport/pico/PicoQuicStatsCallback.h>
#include <moxygen/openmoq/transport/pico/PicoTransportConfig.h>
#include <memory>
#include <string>
#include <vector>

namespace moxygen {

/**
 * MoQPicoQuicShardedServer - shards a picoquic listener across N EventBases.
 *
 * Owns one independent MoQPicoQuicEventBaseServer ("shard") per supplied
 * EventBase, each with its own picoquic_quic_t and UDP socket bound to the
 * same address via SO_REUSEPORT, so the kernel load-balances by 4-tuple hash.
 * Subclasses override the same virtuals they would for
 * MoQPicoQuicEventBaseServer — each shard forwards them here.
 *
 * More than one shard forces QUIC connection migration off: SO_REUSEPORT
 * cannot route a migrated connection's packets to the shard holding its
 * state. A single shard behaves exactly like MoQPicoQuicEventBaseServer.
 */
class MoQPicoQuicShardedServer : public MoQServerBase {
 public:
  MoQPicoQuicShardedServer(
      std::string cert,
      std::string key,
      std::string endpoint,
      std::string versions = "",
      PicoTransportConfig transportConfig = {},
      PicoWebTransportConfig wtConfig = {});
  ~MoQPicoQuicShardedServer() override;

  MoQPicoQuicShardedServer(const MoQPicoQuicShardedServer&) = delete;
  MoQPicoQuicShardedServer(MoQPicoQuicShardedServer&&) = delete;
  MoQPicoQuicShardedServer& operator=(const MoQPicoQuicShardedServer&) = delete;
  MoQPicoQuicShardedServer& operator=(MoQPicoQuicShardedServer&&) = delete;

  void start(const folly::SocketAddress& addr) override {
    start(addr, {});
  }

  /**
   * Binds addr across evbs, one shard per EventBase. An empty evbs spins up
   * one internally-owned thread. Call once. Each shard's picoquic context is
   * pinned to the thread that runs its EventBase, so an evb passed here must
   * not move to another thread afterwards. Throws if a shard fails to bind.
   */
  void start(
      const folly::SocketAddress& addr,
      std::vector<folly::EventBase*> evbs);

  // Blocks until every shard's in-flight sessions have drained. Must not be
  // called from a shard's EventBase thread.
  void stop() override;

  [[nodiscard]] folly::SocketAddress getAddress() const override {
    return boundAddr_;
  }

  [[nodiscard]] size_t numShards() const noexcept {
    return shards_.size();
  }

  /**
   * Invoked once per shard, on that shard's EventBase, immediately before that
   * shard binds. Must be set before start().
   */
  void setPicoQuicStatsCallbackFactory(
      folly::Function<std::shared_ptr<PicoQuicStatsCallback>(folly::EventBase*)>
          factory) {
    statsFactory_ = std::move(factory);
  }

 private:
  class ShardServer;
  friend class ShardServer;

  struct Shard {
    folly::EventBase* evb{nullptr};
    std::shared_ptr<ShardServer> server;
    // Posted by server's deleter, after the last base destructor has run.
    std::shared_ptr<folly::Baton<>> destroyed;
  };

  void teardown();

  bool isInWorkerPool() const noexcept {
    for (const auto& shard : shards_) {
      if (shard.evb->isInEventBaseThread()) {
        return true;
      }
    }
    return false;
  }

  std::string cert_;
  std::string key_;
  std::string endpoint_;
  std::string versions_;
  PicoTransportConfig transportConfig_;
  PicoWebTransportConfig wtConfig_;
  folly::Function<std::shared_ptr<PicoQuicStatsCallback>(folly::EventBase*)>
      statsFactory_;

  std::vector<std::unique_ptr<folly::ScopedEventBaseThread>> ownedWorkers_;
  std::vector<Shard> shards_;
  folly::SocketAddress boundAddr_;
  bool started_{false};
  bool stopped_{false};
};

} // namespace moxygen
