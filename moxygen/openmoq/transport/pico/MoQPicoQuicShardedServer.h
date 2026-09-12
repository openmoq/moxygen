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
 * same address via SO_REUSEPORT (kernel load-balances by 4-tuple hash).
 * Subclasses (e.g. an app's relay server) override createSession()/
 * onNewSession()/terminateClientSession() exactly as they would for
 * MoQPicoQuicEventBaseServer — each shard forwards those calls here.
 *
 * With more than one shard, QUIC connection migration is forced off:
 * plain SO_REUSEPORT cannot route a migrated connection's packets to the
 * shard holding its state. A single shard behaves identically to a plain
 * MoQPicoQuicEventBaseServer (no SO_REUSEPORT, migration untouched).
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
  MoQPicoQuicShardedServer& operator=(const MoQPicoQuicShardedServer&) =
      delete;
  MoQPicoQuicShardedServer& operator=(MoQPicoQuicShardedServer&&) = delete;

  // Satisfies MoQServerBase's pure virtual; spins up a single internally
  // owned shard/thread.
  void start(const folly::SocketAddress& addr) override {
    start(addr, {});
  }

  /**
   * Binds addr across evbs (one shard per EventBase). An empty evbs spins up
   * one internally-owned thread. Must be called from the same thread as the
   * subsequent stop() and only once.
   */
  void start(const folly::SocketAddress& addr, std::vector<folly::EventBase*> evbs);

  void stop() override;

  [[nodiscard]] folly::SocketAddress getAddress() const override {
    return boundAddr_;
  }

  [[nodiscard]] size_t numShards() const noexcept {
    return shards_.size();
  }

  /**
   * Invoked once per shard, on that shard's EventBase, immediately before
   * that shard starts accepting connections. Must be set before start().
   */
  void setPicoQuicStatsCallbackFactory(
      folly::Function<std::shared_ptr<PicoQuicStatsCallback>(folly::EventBase*)>
          factory) {
    statsFactory_ = std::move(factory);
  }

 private:
  class ShardServer;
  friend class ShardServer;

  std::string cert_;
  std::string key_;
  std::string endpoint_;
  std::string versions_;
  PicoTransportConfig transportConfig_;
  PicoWebTransportConfig wtConfig_;
  folly::Function<std::shared_ptr<PicoQuicStatsCallback>(folly::EventBase*)>
      statsFactory_;

  std::vector<folly::EventBase*> workerEvbs_;
  std::vector<std::unique_ptr<folly::ScopedEventBaseThread>> ownedWorkers_;
  // shared_ptr: handleClientSession's shared_from_this() keep-alive silently
  // no-ops for a unique_ptr-owned server, letting stop() free a shard out
  // from under a still-running session coroutine.
  std::vector<std::shared_ptr<ShardServer>> shards_;
  folly::SocketAddress boundAddr_;
  bool started_{false};
  bool stopped_{false};
};

} // namespace moxygen
