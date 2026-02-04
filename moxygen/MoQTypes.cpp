/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <moxygen/MoQTypes.h>

#include <moxygen/compat/Containers.h>
#include <moxygen/compat/Debug.h>
#include <sstream>

namespace {

const char* getFrameTypeString(moxygen::FrameType type) {
  switch (type) {
    case moxygen::FrameType::LEGACY_CLIENT_SETUP:
      return "LEGACY_CLIENT_SETUP";
    case moxygen::FrameType::LEGACY_SERVER_SETUP:
      return "LEGACY_SERVER_SETUP";
    case moxygen::FrameType::SUBSCRIBE:
      return "SUBSCRIBE";
    case moxygen::FrameType::SUBSCRIBE_OK:
      return "SUBSCRIBE_OK";
    case moxygen::FrameType::SUBSCRIBE_ERROR:
      return "SUBSCRIBE_ERROR";
    case moxygen::FrameType::SUBSCRIBE_DONE:
      return "SUBSCRIBE_DONE";
    case moxygen::FrameType::MAX_REQUEST_ID:
      return "MAX_REQUEST_ID";
    case moxygen::FrameType::UNSUBSCRIBE:
      return "UNSUBSCRIBE";
    case moxygen::FrameType::PUBLISH_NAMESPACE:
      return "PUBLISH_NAMESPACE";
    case moxygen::FrameType::PUBLISH_NAMESPACE_OK:
      return "PUBLISH_NAMESPACE_OK";
    case moxygen::FrameType::PUBLISH_NAMESPACE_ERROR:
      return "PUBLISH_NAMESPACE_ERROR";
    case moxygen::FrameType::PUBLISH_NAMESPACE_DONE:
      return "PUBLISH_NAMESPACE_DONE";
    case moxygen::FrameType::GOAWAY:
      return "GOAWAY";
    default:
      // can happen when type was cast from uint8_t
      return "Unknown";
  }
}

const char* getStreamTypeString(moxygen::StreamType type) {
  switch (type) {
    case moxygen::StreamType::SUBGROUP_HEADER_SG:
    case moxygen::StreamType::SUBGROUP_HEADER_SG_EXT:
    case moxygen::StreamType::SUBGROUP_HEADER_SG_FIRST:
    case moxygen::StreamType::SUBGROUP_HEADER_SG_FIRST_EXT:
    case moxygen::StreamType::SUBGROUP_HEADER_SG_ZERO:
    case moxygen::StreamType::SUBGROUP_HEADER_SG_ZERO_EXT:
      return "SUBGROUP_HEADER";
    case moxygen::StreamType::FETCH_HEADER:
      return "FETCH_HEADER";
    default:
      // can happen when type was cast from uint8_t
      return "Unknown";
  }
}

const char* getObjectStatusString(moxygen::ObjectStatus objectStatus) {
  switch (objectStatus) {
    case moxygen::ObjectStatus::NORMAL:
      return "NORMAL";
    case moxygen::ObjectStatus::OBJECT_NOT_EXIST:
      return "OBJECT_NOT_EXIST";
    case moxygen::ObjectStatus::GROUP_NOT_EXIST:
      return "GROUP_NOT_EXIST";
    case moxygen::ObjectStatus::END_OF_GROUP:
      return "END_OF_GROUP";
    case moxygen::ObjectStatus::END_OF_TRACK:
      return "END_OF_TRACK";
    default:
      // can happen when type was cast from uint8_t
      return "Unknown";
  }
}

// Helper to split string by delimiter
std::vector<std::string> splitString(
    const std::string& str,
    const std::string& delimiter) {
  std::vector<std::string> result;
  size_t start = 0;
  size_t end = str.find(delimiter);
  while (end != std::string::npos) {
    result.push_back(str.substr(start, end - start));
    start = end + delimiter.length();
    end = str.find(delimiter, start);
  }
  result.push_back(str.substr(start));
  return result;
}

} // namespace

namespace moxygen {

std::string AbsoluteLocation::describe() const {
  std::ostringstream oss;
  oss << "{" << group << "," << object << "}";
  return oss.str();
}

TrackNamespace::TrackNamespace(std::string tns, std::string delimiter) {
  trackNamespace = splitString(tns, delimiter);
}

std::string FullTrackName::describe() const {
  if (trackNamespace.empty()) {
    return trackName;
  }
  return trackNamespace.describe() + "/" + trackName;
}

std::string toString(LocationType loctype) {
  switch (loctype) {
    case LocationType::NextGroupStart: {
      return "NextGroupStart";
    }
    case LocationType::AbsoluteStart: {
      return "AbsoluteStart";
    }
    case LocationType::LargestObject: {
      return "LargestObject";
    }
    case LocationType::AbsoluteRange: {
      return "AbsoluteRange";
    }
    case LocationType::LargestGroup: {
      return "LargestGroup";
    }
    default: {
      return "Unknown";
    }
  }
}

std::ostream& operator<<(std::ostream& os, FrameType type) {
  os << getFrameTypeString(type);
  return os;
}

std::ostream& operator<<(std::ostream& os, StreamType type) {
  os << getStreamTypeString(type);
  return os;
}

std::ostream& operator<<(std::ostream& os, ObjectStatus status) {
  os << getObjectStatusString(status);
  return os;
}

std::ostream& operator<<(std::ostream& os, TrackAlias alias) {
  os << alias.value;
  return os;
}

std::ostream& operator<<(std::ostream& os, RequestID id) {
  os << id.value;
  return os;
}

std::ostream& operator<<(std::ostream& os, const ObjectHeader& header) {
  os << " group=" << header.group << " subgroup=" << header.subgroup
     << " id=" << header.id << " priority="
     << (header.priority.has_value()
             ? std::to_string(uint32_t(header.priority.value()))
             : "none")
     << " status=" << getObjectStatusString(header.status) << " length="
     << (header.length.has_value() ? std::to_string(header.length.value())
                                   : "none");
  return os;
}

//// Parameters ////

// Frame type sets for parameter allowlist
const compat::FastSet<FrameType> kAllowedFramesForAuthToken = {
    FrameType::PUBLISH,
    FrameType::SUBSCRIBE,
    FrameType::SUBSCRIBE_UPDATE,
    FrameType::SUBSCRIBE_NAMESPACE,
    FrameType::PUBLISH_NAMESPACE,
    FrameType::TRACK_STATUS,
    FrameType::FETCH};

const compat::FastSet<FrameType> kAllowedFramesForDeliveryTimeout = {
    FrameType::PUBLISH_OK,
    FrameType::SUBSCRIBE,
    FrameType::SUBSCRIBE_UPDATE};

const compat::FastSet<FrameType> kAllowedFramesForSubscriberPriority = {
    FrameType::SUBSCRIBE,
    FrameType::FETCH,
    FrameType::SUBSCRIBE_UPDATE,
    FrameType::PUBLISH_OK};

const compat::FastSet<FrameType> kAllowedFramesForSubscriptionFilter = {
    FrameType::SUBSCRIBE,
    FrameType::PUBLISH_OK,
    FrameType::SUBSCRIBE_UPDATE};

const compat::FastSet<FrameType> kAllowedFramesForExpires = {
    FrameType::SUBSCRIBE_OK,
    FrameType::PUBLISH,
    FrameType::PUBLISH_OK,
    FrameType::REQUEST_OK};

const compat::FastSet<FrameType> kAllowedFramesForGroupOrder = {
    FrameType::SUBSCRIBE,
    FrameType::PUBLISH_OK,
    FrameType::FETCH};

const compat::FastSet<FrameType> kAllowedFramesForLargestObject = {
    FrameType::SUBSCRIBE_OK,
    FrameType::PUBLISH,
    FrameType::REQUEST_OK};

const compat::FastSet<FrameType> kAllowedFramesForForward = {
    FrameType::SUBSCRIBE,
    FrameType::SUBSCRIBE_UPDATE,
    FrameType::PUBLISH,
    FrameType::PUBLISH_OK,
    FrameType::SUBSCRIBE_NAMESPACE};

// Allowlist mapping: TrackRequestParamKey -> set of allowed FrameTypes
// Empty set means allowed for all frame types
const compat::FastMap<TrackRequestParamKey, compat::FastSet<FrameType>>
    kParamAllowlist = {
        {TrackRequestParamKey::AUTHORIZATION_TOKEN, kAllowedFramesForAuthToken},
        {TrackRequestParamKey::DELIVERY_TIMEOUT,
         kAllowedFramesForDeliveryTimeout},
        {TrackRequestParamKey::MAX_CACHE_DURATION, {}},
        {TrackRequestParamKey::PUBLISHER_PRIORITY, {}},
        {TrackRequestParamKey::SUBSCRIBER_PRIORITY,
         kAllowedFramesForSubscriberPriority},
        {TrackRequestParamKey::SUBSCRIPTION_FILTER,
         kAllowedFramesForSubscriptionFilter},
        {TrackRequestParamKey::EXPIRES, kAllowedFramesForExpires},
        {TrackRequestParamKey::GROUP_ORDER, kAllowedFramesForGroupOrder},
        {TrackRequestParamKey::LARGEST_OBJECT, kAllowedFramesForLargestObject},
        {TrackRequestParamKey::FORWARD, kAllowedFramesForForward},
};

// Frame types that allow all parameters (no validation)
const compat::FastSet<FrameType> kAllowAllParamsFrameTypes = {
    FrameType::CLIENT_SETUP,
    FrameType::SERVER_SETUP,
    FrameType::LEGACY_CLIENT_SETUP,
    FrameType::LEGACY_SERVER_SETUP,
};

bool Parameters::isParamAllowed(TrackRequestParamKey key) const {
  // Setup frame types allow all parameters
  if (kAllowAllParamsFrameTypes.contains(frameType_)) {
    return true;
  }

  auto it = kParamAllowlist.find(key);
  if (it == kParamAllowlist.end()) {
    return false;
  }

  const auto& allowedFrameTypes = it->second;
  if (allowedFrameTypes.empty()) {
    // Empty set means allowed for all frame types
    return true;
  }

  return allowedFrameTypes.contains(frameType_);
}

Extensions noExtensions() {
  return Extensions();
}

std::pair<StandaloneFetch*, JoiningFetch*> fetchType(Fetch& fetch) {
  auto standalone = std::get_if<StandaloneFetch>(&fetch.args);
  auto joining = std::get_if<JoiningFetch>(&fetch.args);
  return {standalone, joining};
}

std::pair<const StandaloneFetch*, const JoiningFetch*> fetchType(
    const Fetch& fetch) {
  auto standalone = std::get_if<StandaloneFetch>(&fetch.args);
  auto joining = std::get_if<JoiningFetch>(&fetch.args);
  return {standalone, joining};
}

SubscribeRequest SubscribeRequest::make(
    const FullTrackName& fullTrackName,
    uint8_t priority,
    GroupOrder groupOrder,
    bool forward,
    LocationType locType,
    std::optional<AbsoluteLocation> start,
    uint64_t endGroup,
    const std::vector<Parameter>& inputParams) {
  SubscribeRequest req = SubscribeRequest{
      RequestID(), // Default constructed RequestID
      fullTrackName,
      priority,
      groupOrder,
      forward,
      locType,
      std::move(start),
      endGroup,
      TrackRequestParameters{FrameType::SUBSCRIBE}};

  for (const auto& param : inputParams) {
    auto result = req.params.insertParam(param);
    if (result.hasError()) {
      LOG(ERROR) << "SubscribeRequest::make: param not allowed, key="
                 << param.key;
    }
  }
  return req;
}

Fetch::Fetch(
    RequestID su,
    FullTrackName ftn,
    AbsoluteLocation st,
    AbsoluteLocation e,
    uint8_t p,
    GroupOrder g,
    const std::vector<Parameter>& pa)
    : requestID(su),
      fullTrackName(std::move(ftn)),
      priority(p),
      groupOrder(g),
      args(StandaloneFetch(st, e)) {
  for (const auto& param : pa) {
    auto result = params.insertParam(param);
    if (result.hasError()) {
      LOG(ERROR) << "Fetch: param not allowed, key=" << param.key;
    }
  }
}

Fetch::Fetch(
    RequestID su,
    RequestID jsid,
    uint64_t joiningStart,
    FetchType fetchType,
    uint8_t p,
    GroupOrder g,
    const std::vector<Parameter>& pa)
    : requestID(su),
      priority(p),
      groupOrder(g),
      args(JoiningFetch(jsid, joiningStart, fetchType)) {
  CHECK(
      fetchType == FetchType::RELATIVE_JOINING ||
      fetchType == FetchType::ABSOLUTE_JOINING);
  for (const auto& param : pa) {
    auto result = params.insertParam(param);
    if (result.hasError()) {
      LOG(ERROR) << "Fetch: param not allowed, key=" << param.key;
    }
  }
}

} // namespace moxygen
