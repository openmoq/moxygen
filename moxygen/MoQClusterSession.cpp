/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <moxygen/MoQClusterSession.h>

#include <array>
#include <deque>
#include <functional>
#include <utility>

namespace moxygen {
class ClusterNamespaceOwner;

// A namespace has one advertisement stream owner in each session direction.
// Owners outlive asynchronous application callbacks, but reset permanently when
// their stream ends so retained handles cannot claim namespaces again.
struct ClusterNamespaceRegistry {
  bool active{true};
  folly::F14FastMap<
      TrackNamespace,
      std::array<const ClusterNamespaceOwner*, 2>,
      TrackNamespace::hash>
      owners;
  // Retry callbacks from claim() losers, keyed by namespace and direction.
  folly::F14FastMap<
      TrackNamespace,
      std::array<std::deque<std::function<void()>>, 2>,
      TrackNamespace::hash>
      waiters;
};

class ClusterNamespaceOwner : public NamespaceAdvertisement {
 public:
  ClusterNamespaceOwner(
      std::shared_ptr<ClusterNamespaceRegistry> registry,
      bool incoming,
      TrackNamespace prefix)
      : registry_(std::move(registry)),
        prefix_(std::move(prefix)),
        incoming_(incoming) {}
  ClusterNamespaceOwner(const ClusterNamespaceOwner&) = delete;
  ClusterNamespaceOwner& operator=(const ClusterNamespaceOwner&) = delete;
  ~ClusterNamespaceOwner() {
    reset();
  }

  bool claim(const TrackNamespace& suffix) override {
    if (!active_ || !registry_->active) {
      return false;
    }
    auto full = fullNamespace(suffix);
    auto& owners = registry_->owners[full];
    auto& owner = owners[incoming_];
    if (owner && owner != this) {
      return false;
    }
    owner = this;
    claimed_.insert(std::move(full));
    return true;
  }
  bool owns(const TrackNamespace& suffix) const override {
    auto it = registry_->owners.find(fullNamespace(suffix));
    return active_ && registry_->active && it != registry_->owners.end() &&
        it->second[incoming_] == this;
  }
  bool release(const TrackNamespace& suffix) override {
    if (!owns(suffix)) {
      return false;
    }
    auto full = fullNamespace(suffix);
    releaseFull(full);
    claimed_.erase(full);
    return true;
  }
  void reset() override {
    active_ = false;
    for (const auto& full : claimed_) {
      releaseFull(full);
    }
    claimed_.clear();
  }
  void setPrefix(TrackNamespace prefix) override {
    prefix_ = std::move(prefix);
    // Release claims that no longer fall under the narrowed prefix.
    for (auto it = claimed_.begin(); it != claimed_.end();) {
      if (!it->startsWith(prefix_)) {
        releaseFull(*it);
        it = claimed_.erase(it);
      } else {
        ++it;
      }
    }
  }
  void queuePrefix(std::optional<TrackNamespace> prefix) override {
    pendingPrefixes_.push_back(std::move(prefix));
  }
  void acceptPrefix() override {
    if (pendingPrefixes_.empty()) {
      return;
    }
    if (pendingPrefixes_.front()) {
      setPrefix(std::move(*pendingPrefixes_.front()));
    }
    pendingPrefixes_.pop_front();
  }
  void discardPendingPrefix() override {
    if (!pendingPrefixes_.empty()) {
      pendingPrefixes_.pop_back();
    }
  }
  void retryClaim(const TrackNamespace& suffix, std::function<void()> callback)
      override {
    registry_->waiters[fullNamespace(suffix)][incoming_].push_back(
        std::move(callback));
  }

 private:
  TrackNamespace fullNamespace(const TrackNamespace& suffix) const {
    auto full = prefix_;
    for (const auto& token : suffix.trackNamespace) {
      full.append(token);
    }
    return full;
  }
  void releaseFull(const TrackNamespace& full) {
    auto it = registry_->owners.find(full);
    if (it != registry_->owners.end()) {
      if (it->second[incoming_] == this) {
        it->second[incoming_] = nullptr;
      }
      if (!it->second[0] && !it->second[1]) {
        registry_->owners.erase(it);
      }
    }
    notifyWaiters(full);
  }
  void notifyWaiters(const TrackNamespace& full) {
    auto wit = registry_->waiters.find(full);
    if (wit == registry_->waiters.end()) {
      return;
    }
    std::function<void()> callback;
    auto& perDirection = wit->second;
    if (!perDirection[incoming_].empty()) {
      callback = std::move(perDirection[incoming_].front());
      perDirection[incoming_].pop_front();
    }
    if (perDirection[0].empty() && perDirection[1].empty()) {
      registry_->waiters.erase(wit);
    }
    if (callback) {
      callback();
    }
  }
  std::shared_ptr<ClusterNamespaceRegistry> registry_;
  TrackNamespace prefix_;
  bool active_{true};
  bool incoming_;
  std::deque<std::optional<TrackNamespace>> pendingPrefixes_;
  folly::F14FastSet<TrackNamespace, TrackNamespace::hash> claimed_;
};

std::shared_ptr<NamespaceAdvertisement>
MoQClusterSession::makeNamespaceAdvertisement(
    bool incoming,
    TrackNamespace prefix) {
  if (!negotiatedSetupExtension(SetupExtension::RelayHops)) {
    return nullptr;
  }
  if (!namespaceRegistry_) {
    namespaceRegistry_ = std::make_shared<ClusterNamespaceRegistry>();
  }
  return std::make_shared<ClusterNamespaceOwner>(
      namespaceRegistry_, incoming, std::move(prefix));
}

void MoQClusterSession::cleanupClusterState() {
  if (namespaceRegistry_) {
    namespaceRegistry_->active = false;
    namespaceRegistry_->owners.clear();
    namespaceRegistry_->waiters.clear();
  }
}

MoQClusterSession::~MoQClusterSession() {
  cleanupClusterState();
}

void MoQClusterSession::cleanup() {
  cleanupClusterState();
  MoQRelaySession::cleanup();
}

std::function<std::shared_ptr<MoQSession>(
    folly::MaybeManagedPtr<proxygen::WebTransport>,
    std::shared_ptr<MoQExecutor>)>
MoQClusterSession::createClusterSessionFactory() {
  return [](folly::MaybeManagedPtr<proxygen::WebTransport> wt,
            std::shared_ptr<MoQExecutor> exec) {
    return std::make_shared<MoQClusterSession>(wt, std::move(exec));
  };
}
} // namespace moxygen
