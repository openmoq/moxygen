/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <moxygen/openmoq/extensions/MoQClusterExtension.h>

#include <folly/container/F14Set.h>
#include <moxygen/MoQSession.h>
#include <moxygen/MoQVarint.h>
#include <quic/folly_utils/Utils.h>

namespace moxygen {

namespace {

constexpr uint64_t kMinClusterDraft = 18;

bool clusterApplies(uint64_t version) {
  return getDraftMajorVersion(version) >= kMinClusterDraft;
}

size_t countParams(const SetupParameters& params, uint64_t key) {
  return std::count_if(
      params.begin(), params.end(), [key](const Parameter& param) {
        return param.key == key;
      });
}

} // namespace

folly::Expected<std::string, ErrorCode> encodeRelayHopPath(
    const std::vector<uint64_t>& hopPath,
    uint64_t version) noexcept {
  if (hopPath.empty()) {
    return folly::makeUnexpected(ErrorCode::PROTOCOL_VIOLATION);
  }

  folly::IOBufQueue encoded{folly::IOBufQueue::cacheChainLength()};
  MoQFrameWriter writer;
  writer.initializeVersion(version);
  size_t size = 0;
  bool error = false;
  folly::F14FastSet<uint64_t> seen;
  for (const auto hop : hopPath) {
    if (hop != kMoQClusterAnonHopId && !seen.insert(hop).second) {
      return folly::makeUnexpected(ErrorCode::PROTOCOL_VIOLATION);
    }
    writer.writeVarint(encoded, hop, size, error);
    if (error) {
      return folly::makeUnexpected(ErrorCode::PROTOCOL_VIOLATION);
    }
  }
  return encoded.move()->moveToFbString().toStdString();
}

folly::Expected<std::vector<uint64_t>, ErrorCode> decodeRelayHopPath(
    std::string_view encoded,
    uint64_t version) noexcept {
  if (encoded.empty()) {
    return folly::makeUnexpected(ErrorCode::PROTOCOL_VIOLATION);
  }

  auto buffer = folly::IOBuf::wrapBuffer(encoded.data(), encoded.size());
  folly::io::Cursor cursor(buffer.get());
  size_t remaining = encoded.size();
  std::vector<uint64_t> hopPath;
  folly::F14FastSet<uint64_t> seen;
  while (remaining > 0) {
    const auto decoded = getDraftMajorVersion(version) >= 17
        ? decodeMoQVarint(cursor, remaining)
        : quic::follyutils::decodeQuicInteger(cursor, remaining);
    if (!decoded) {
      return folly::makeUnexpected(ErrorCode::PROTOCOL_VIOLATION);
    }
    if (decoded->first != kMoQClusterAnonHopId &&
        !seen.insert(decoded->first).second) {
      return folly::makeUnexpected(ErrorCode::PROTOCOL_VIOLATION);
    }
    hopPath.push_back(decoded->first);
    remaining -= decoded->second;
  }
  return hopPath;
}

std::vector<SetupParameter> MoQClusterExtension::getSetupOptions(
    uint64_t version) const {
  if (!clusterApplies(version)) {
    return {};
  }
  std::vector<SetupParameter> options{SetupParameter(kHopIdOption, hopID_)};
  if (relayCost_) {
    options.emplace_back(kRelayCostOption, *relayCost_);
  }
  return options;
}

folly::Expected<MessageParameterExtensions, SessionCloseErrorCode>
MoQClusterExtension::negotiate(
    uint64_t version,
    const SetupParameters& localParams,
    const SetupParameters& peerParams) const {
  if (!clusterApplies(version)) {
    return MessageParameterExtensions{};
  }
  if (countParams(peerParams, kHopIdOption) > 1 ||
      countParams(peerParams, kRelayCostOption) > 1) {
    return folly::makeUnexpected(SessionCloseErrorCode::PROTOCOL_VIOLATION);
  }
  if (!localParams.hasParam(kHopIdOption) ||
      !peerParams.hasParam(kHopIdOption)) {
    return MessageParameterExtensions{};
  }

  const std::vector<FrameType> advertisements{
      FrameType::PUBLISH_NAMESPACE,
      FrameType::NAMESPACE,
      FrameType::REQUEST_UPDATE};
  MessageParameterExtensions rules;
  rules.emplace(
      kHopPathParam,
      MessageParameterExtension{
          .encoding = ParameterValueEncoding::LengthPrefixed,
          .allowedFrames = advertisements,
          .requiredFrames = {FrameType::PUBLISH_NAMESPACE, FrameType::NAMESPACE},
          .repeatable = false,
          .validate =
              [](uint64_t version, const Parameter& param, ParameterDirection) {
                return decodeRelayHopPath(param.asString, version).hasValue();
              }});
  rules.emplace(
      kRouteCostParam,
      MessageParameterExtension{
          .encoding = ParameterValueEncoding::Varint,
          .allowedFrames = advertisements,
          .repeatable = false});
  return rules;
}

bool MoQClusterExtension::isNegotiated(const MoQSession& session) {
  return session.getMessageParameterExtensions().count(kHopPathParam) > 0;
}

namespace {

const Parameter* peerOption(const MoQSession& session, uint64_t key) {
  const auto& peer = session.getPeerSetupParams();
  if (!MoQClusterExtension::isNegotiated(session) || !peer) {
    return nullptr;
  }
  return peer->getFirstParam(key);
}

} // namespace

uint64_t MoQClusterExtension::peerHopID(const MoQSession& session) {
  const auto* hop = peerOption(session, kHopIdOption);
  return hop ? hop->asUint64 : kMoQClusterAnonHopId;
}

uint64_t MoQClusterExtension::linkCost(const MoQSession& session) {
  const auto* cost = peerOption(session, kRelayCostOption);
  return cost ? cost->asUint64 : 1;
}

} // namespace moxygen
