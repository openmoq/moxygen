/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <moxygen/MoQExtension.h>

#include <optional>

namespace moxygen {

// Selects the maxSelected tracks in a namespace with the highest value of
// propertyType.
struct TrackFilter {
  uint64_t propertyType{0};
  uint64_t maxSelected{0};

  bool operator==(const TrackFilter& other) const = default;
};

// The Top Tracks Filter from moq-transport#1981, on draft 16 and later. Each
// endpoint declares MAX_TOP_TRACKS, the largest MaxTopTracks it accepts, and
// may send TOP_TRACKS_FILTER only up to the limit its peer declared. The filter
// rides on SUBSCRIBE_TRACKS from draft 18 and on SUBSCRIBE_NAMESPACE before
// it, and on REQUEST_UPDATE. The framer matches REQUEST_UPDATE by wire type, so
// it accepts the filter on an update for any request; the application decides
// what it means there.
//
//   client.addExtension(std::make_shared<TopTracksExtension>(0));
//   subTracks.params.insertParam(
//       TopTracksExtension::encode(TrackFilter{kAudioLevel, 3}, version));
class TopTracksExtension : public MoQExtension {
 public:
  // 0x09 in moq-transport#1981, which is odd, so the KVP rules give it a
  // length-prefixed value rather than an integer. This even placeholder stands
  // in until the draft settles on a codepoint.
  static constexpr uint64_t kMaxTopTracksOption = 0x0A;
  // 0x30 in moq-transport#1981. An even key carries a single varint before
  // draft 18, so this odd one keeps the filter length-prefixed on every draft.
  static constexpr uint64_t kTopTracksFilterParam = 0x31;

  // 0 accepts no filter from the peer.
  explicit TopTracksExtension(uint64_t maxTopTracks)
      : maxTopTracks_(maxTopTracks) {}

  std::vector<SetupParameter> getSetupOptions(uint64_t version) const override;

  folly::Expected<MessageParameterExtensions, SessionCloseErrorCode> negotiate(
      uint64_t version,
      const SetupParameters& localParams,
      const SetupParameters& peerParams) const override;

  // Values past the draft's varint range are clipped to its largest value.
  static Parameter encode(const TrackFilter& filter, uint64_t version);
  // An empty value is no filter, which REQUEST_UPDATE uses to remove one.
  static Parameter encodeRemoval();
  // nullopt for a malformed or empty value.
  static std::optional<TrackFilter> decode(
      const Parameter& param,
      uint64_t version);
  // The filter in `params`, or nullopt if it has none or an empty one.
  static std::optional<TrackFilter> find(
      const TrackRequestParameters& params,
      uint64_t version);

 private:
  uint64_t maxTopTracks_;
};

} // namespace moxygen
