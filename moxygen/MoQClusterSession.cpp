/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <moxygen/MoQClusterSession.h>

#include <array>
#include <deque>
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
    auto& owners = registry_->owners[fullNamespace(suffix)];
    auto& owner = owners[incoming_];
    if (owner && owner != this) {
      return false;
    }
    owner = this;
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
    auto it = registry_->owners.find(fullNamespace(suffix));
    it->second[incoming_] = nullptr;
    if (!it->second[0] && !it->second[1]) {
      registry_->owners.erase(it);
    }
    return true;
  }
  void reset() override {
    active_ = false;
    for (auto it = registry_->owners.begin(); it != registry_->owners.end();) {
      if (it->second[incoming_] == this) {
        it->second[incoming_] = nullptr;
      }
      if (!it->second[0] && !it->second[1]) {
        it = registry_->owners.erase(it);
      } else {
        ++it;
      }
    }
  }
  void setPrefix(TrackNamespace prefix) override {
    prefix_ = std::move(prefix);
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

 private:
  TrackNamespace fullNamespace(const TrackNamespace& suffix) const {
    auto full = prefix_;
    for (const auto& token : suffix.trackNamespace) {
      full.append(token);
    }
    return full;
  }
  std::shared_ptr<ClusterNamespaceRegistry> registry_;
  TrackNamespace prefix_;
  bool active_{true};
  bool incoming_;
  std::deque<std::optional<TrackNamespace>> pendingPrefixes_;
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
