/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <moxygen/openmoq/extensions/TopTracksExtension.h>

#include <moxygen/MoQVarint.h>
#include <quic/folly_utils/Utils.h>

#include <algorithm>
#include <limits>

namespace moxygen {

namespace {

constexpr uint64_t kMinTopTracksDraft = 16;

bool topTracksApplies(uint64_t version) {
  return getDraftMajorVersion(version) >= kMinTopTracksDraft;
}

uint64_t declaredLimit(const SetupParameters& params) {
  const auto* param =
      params.getFirstParam(TopTracksExtension::kMaxTopTracksOption);
  return param ? param->asUint64 : 0;
}

} // namespace

std::vector<SetupParameter> TopTracksExtension::getSetupOptions(
    uint64_t version) const {
  if (!topTracksApplies(version) || maxTopTracks_ == 0) {
    return {};
  }
  return {SetupParameter(kMaxTopTracksOption, maxTopTracks_)};
}

folly::Expected<MessageParameterExtensions, SessionCloseErrorCode>
TopTracksExtension::negotiate(
    uint64_t version,
    const SetupParameters& localParams,
    const SetupParameters& peerParams) const {
  const auto receiveLimit = declaredLimit(localParams);
  const auto sendLimit = declaredLimit(peerParams);
  if (!topTracksApplies(version) || (receiveLimit == 0 && sendLimit == 0)) {
    return MessageParameterExtensions{};
  }
  // Draft 18 split track discovery out of SUBSCRIBE_NAMESPACE.
  const auto subscribeFrame = getDraftMajorVersion(version) >= 18
      ? FrameType::SUBSCRIBE_TRACKS
      : FrameType::SUBSCRIBE_NAMESPACE;
  MessageParameterExtensions rules;
  rules.emplace(
      kTopTracksFilterParam,
      MessageParameterExtension{
          .encoding = ParameterValueEncoding::LengthPrefixed,
          .allowedFrames = {subscribeFrame, FrameType::REQUEST_UPDATE},
          .repeatable = false,
          .validate = [receiveLimit, sendLimit](
                          uint64_t version,
                          const Parameter& param,
                          ParameterDirection direction) {
            const auto limit = direction == ParameterDirection::Sent
                ? sendLimit
                : receiveLimit;
            if (limit == 0) {
              return false;
            }
            if (param.asString.empty()) {
              return true;
            }
            auto filter = decode(param, version);
            return filter && filter->propertyType % 2 == 0 &&
                filter->maxSelected != 0 && filter->maxSelected <= limit;
          }});
  return rules;
}

Parameter TopTracksExtension::encode(
    const TrackFilter& filter,
    uint64_t version) {
  const auto maxValue = getDraftMajorVersion(version) >= 17
      ? std::numeric_limits<uint64_t>::max()
      : quic::kEightByteLimit;
  folly::IOBufQueue encoded{folly::IOBufQueue::cacheChainLength()};
  MoQFrameWriter writer;
  writer.initializeVersion(version);
  size_t size = 0;
  bool error = false;
  writer.writeVarint(
      encoded, std::min(filter.propertyType, maxValue), size, error);
  writer.writeVarint(
      encoded, std::min(filter.maxSelected, maxValue), size, error);
  XCHECK(!error) << "a clipped TrackFilter value did not encode";
  return Parameter(
      kTopTracksFilterParam,
      encoded.move()->moveToFbString().toStdString());
}

Parameter TopTracksExtension::encodeRemoval() {
  return Parameter(kTopTracksFilterParam, std::string());
}

std::optional<TrackFilter> TopTracksExtension::decode(
    const Parameter& param,
    uint64_t version) {
  if (param.key != kTopTracksFilterParam || param.asString.empty()) {
    return std::nullopt;
  }
  const auto& value = param.asString;
  auto buffer = folly::IOBuf::wrapBuffer(value.data(), value.size());
  folly::io::Cursor cursor(buffer.get());
  size_t remaining = value.size();
  auto next = [&]() -> std::optional<uint64_t> {
    const auto decoded = getDraftMajorVersion(version) >= 17
        ? decodeMoQVarint(cursor, remaining)
        : quic::follyutils::decodeQuicInteger(cursor, remaining);
    if (!decoded) {
      return std::nullopt;
    }
    remaining -= decoded->second;
    return decoded->first;
  };
  auto propertyType = next();
  auto maxSelected = propertyType ? next() : std::nullopt;
  if (!maxSelected || remaining != 0) {
    return std::nullopt;
  }
  return TrackFilter{*propertyType, *maxSelected};
}

std::optional<TrackFilter> TopTracksExtension::find(
    const TrackRequestParameters& params,
    uint64_t version) {
  const auto* param = params.getFirstParam(kTopTracksFilterParam);
  return param ? decode(*param, version) : std::nullopt;
}

} // namespace moxygen
