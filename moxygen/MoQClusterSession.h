/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <moxygen/MoQRelaySession.h>

namespace moxygen {
struct ClusterNamespaceRegistry;

// Opt-in namespace ownership for MoQ Cluster. Link this session only in
// applications that participate in the cluster extension.
class MoQClusterSession : public MoQRelaySession {
 public:
  using MoQRelaySession::MoQRelaySession;
  ~MoQClusterSession() override;
  void cleanup() override;

  static std::function<std::shared_ptr<MoQSession>(
      folly::MaybeManagedPtr<proxygen::WebTransport>,
      std::shared_ptr<MoQExecutor>)>
  createClusterSessionFactory();

 protected:
  std::shared_ptr<NamespaceAdvertisement> makeNamespaceAdvertisement(
      bool incoming,
      TrackNamespace prefix = {}) override;

 private:
  void cleanupClusterState();
  std::shared_ptr<ClusterNamespaceRegistry> namespaceRegistry_;
};
} // namespace moxygen
