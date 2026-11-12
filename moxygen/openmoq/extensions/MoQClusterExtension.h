/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <moxygen/MoQExtension.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace moxygen {

class MoQSession;

// A relay that declares Hop ID 0 is anonymous: it opts out of loop detection,
// so repeated zeros in one HOP_PATH are not a loop.
constexpr uint64_t kMoQClusterAnonHopId = 0;

// HOP_PATH value codec. Both reject an empty path and a repeated nonzero hop.
folly::Expected<std::string, ErrorCode> encodeRelayHopPath(
    const std::vector<uint64_t>& hopPath,
    uint64_t version) noexcept;

folly::Expected<std::vector<uint64_t>, ErrorCode> decodeRelayHopPath(
    std::string_view encoded,
    uint64_t version) noexcept;

// MoQ Cluster (draft-lcurley-moq-cluster-01) on draft 18 and later. Each
// endpoint declares HOP_ID, and the extension is on when both SETUPs carry
// one. Then PUBLISH_NAMESPACE and NAMESPACE must carry one HOP_PATH, and
// those two plus REQUEST_UPDATE may carry one ROUTE_COST.
//
//   server.addExtension(std::make_shared<MoQClusterExtension>(hopID));
//   ...
//   if (MoQClusterExtension::isNegotiated(*session)) {
//     auto peer = MoQClusterExtension::peerHopID(*session);
//   }
class MoQClusterExtension : public MoQExtension {
 public:
  static constexpr uint64_t kHopIdOption = 0x40B54;
  static constexpr uint64_t kRelayCostOption = 0x40B56;
  static constexpr uint64_t kHopPathParam = 0x40B57;
  static constexpr uint64_t kRouteCostParam = 0x40B58;

  // relayCost is what this endpoint charges for what it sends on the link. An
  // endpoint that omits it is priced at 1 by its peer.
  explicit MoQClusterExtension(
      uint64_t hopID,
      std::optional<uint64_t> relayCost = std::nullopt)
      : hopID_(hopID), relayCost_(relayCost) {}

  std::vector<SetupParameter> getSetupOptions(uint64_t version) const override;

  // A repeated HOP_ID or RELAY_COST in the peer's SETUP is a
  // PROTOCOL_VIOLATION.
  folly::Expected<MessageParameterExtensions, SessionCloseErrorCode> negotiate(
      uint64_t version,
      const SetupParameters& localParams,
      const SetupParameters& peerParams) const override;

  // False until both SETUPs are known.
  static bool isNegotiated(const MoQSession& session);
  // kMoQClusterAnonHopId unless the extension is negotiated.
  static uint64_t peerHopID(const MoQSession& session);
  // The cost the peer declared for this link: 1 when it omitted RELAY_COST or
  // the extension is off.
  static uint64_t linkCost(const MoQSession& session);

 private:
  uint64_t hopID_;
  std::optional<uint64_t> relayCost_;
};

} // namespace moxygen
