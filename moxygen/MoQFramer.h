/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <moxygen/compat/BufferIO.h>
#include <moxygen/compat/Debug.h>
#include <moxygen/compat/Hash.h>
#include <moxygen/MoQTokenCache.h>
#include <moxygen/MoQTypes.h>
#include <moxygen/MoQVersions.h>

#if MOXYGEN_USE_FOLLY
#include <folly/io/Cursor.h>
#include <folly/io/IOBufQueue.h>
#include <quic/codec/QuicInteger.h>
#include <quic/folly_utils/Utils.h>
#endif

namespace moxygen {

// Type aliases for buffer operations - conditional on MOXYGEN_USE_FOLLY
#if MOXYGEN_USE_FOLLY
using Cursor = folly::io::Cursor;
using BufQueue = folly::IOBufQueue;
using Buffer = folly::IOBuf;
#else
using Cursor = compat::ByteCursor;
using BufQueue = compat::ByteBufferQueue;
using Buffer = compat::ByteBuffer;
#endif

//////// Constants ////////
const size_t kMaxFrameHeaderSize = 32;

using WriteResult = compat::Expected<size_t, quic::TransportErrorCode>;

void writeVarint(
    BufQueue& buf,
    uint64_t value,
    size_t& size,
    bool& error) noexcept;

inline StreamType getSubgroupStreamType(
    uint64_t version,
    SubgroupIDFormat format,
    bool includeExtensions,
    bool endOfGroup,
    bool priorityPresent = true) {
  auto majorVersion = getDraftMajorVersion(version);
  return StreamType(
      compat::to_underlying(StreamType::SUBGROUP_HEADER_MASK) |
      (format == SubgroupIDFormat::Present ? SG_HAS_SUBGROUP_ID : 0) |
      (format == SubgroupIDFormat::FirstObject ? SG_SUBGROUP_VALUE : 0) |
      (includeExtensions ? SG_HAS_EXTENSIONS : 0) |
      (endOfGroup ? SG_HAS_END_OF_GROUP : 0) |
      (majorVersion >= 15 && !priorityPresent ? SG_PRIORITY_NOT_PRESENT : 0));
}

bool isValidSubgroupType(uint64_t version, uint64_t streamType);

inline SubgroupOptions getSubgroupOptions(
    uint64_t version,
    StreamType streamType) {
  SubgroupOptions options;
  auto streamTypeInt = compat::to_underlying(streamType);
  auto majorVersion = getDraftMajorVersion(version);

  options.hasExtensions = streamTypeInt & SG_HAS_EXTENSIONS;
  options.subgroupIDFormat = streamTypeInt & SG_HAS_SUBGROUP_ID
      ? SubgroupIDFormat::Present
      : (streamTypeInt & SG_SUBGROUP_VALUE) ? SubgroupIDFormat::FirstObject
                                            : SubgroupIDFormat::Zero;
  options.hasEndOfGroup =
      compat::to_underlying(streamType) & SG_HAS_END_OF_GROUP;
  // In Draft 15+, check if priority is not present
  if (majorVersion >= 15) {
    options.priorityPresent = !(streamTypeInt & SG_PRIORITY_NOT_PRESENT);
  }
  return options;
}

bool isValidDatagramType(uint64_t version, uint64_t datagramType);
bool datagramPriorityPresent(uint64_t version, DatagramType datagramType);
bool subgroupPriorityPresent(uint64_t version, StreamType streamType);

inline DatagramType getDatagramType(
    uint64_t version,
    bool status,
    bool includeExtensions,
    bool endOfGroup,
    bool isObjectIdZero,
    bool priorityPresent = true) {
  auto majorVersion = getDraftMajorVersion(version);
  if (majorVersion == 11) {
    return DatagramType(
        (status ? DG_HAS_STATUS_V11 : 0) |
        (includeExtensions ? DG_HAS_EXTENSIONS : 0));
  } else if (status) {
    return DatagramType(
        DG_IS_STATUS | (includeExtensions ? DG_HAS_EXTENSIONS : 0) |
        (isObjectIdZero ? DG_OBJECT_ID_ZERO : 0) |
        (majorVersion >= 15 && !priorityPresent ? DG_PRIORITY_NOT_PRESENT : 0));
  } else {
    return DatagramType(
        (includeExtensions ? DG_HAS_EXTENSIONS : 0) |
        (endOfGroup ? DG_HAS_END_OF_GROUP : 0) |
        (isObjectIdZero ? DG_OBJECT_ID_ZERO : 0) |
        (majorVersion >= 15 && !priorityPresent ? DG_PRIORITY_NOT_PRESENT : 0));
  }
}

compat::Expected<std::string, ErrorCode> parseFixedString(
    Cursor& cursor,
    size_t& length);

class MoQFrameParser {
 public:
  template <typename T>
  struct ParseResultAndLength {
    T value;
    size_t bytesConsumed;
  };
  compat::Expected<ClientSetup, ErrorCode> parseClientSetup(
      Cursor& cursor,
      size_t length) noexcept;

  compat::Expected<ServerSetup, ErrorCode> parseServerSetup(
      Cursor& cursor,
      size_t length) noexcept;

  // datagram only
  compat::Expected<DatagramObjectHeader, ErrorCode> parseDatagramObjectHeader(
      Cursor& cursor,
      DatagramType datagramType,
      size_t& length) const noexcept;

  compat::Expected<ParseResultAndLength<RequestID>, ErrorCode> parseFetchHeader(
      Cursor& cursor,
      size_t length) const noexcept;

  struct SubgroupHeaderResult {
    TrackAlias trackAlias;
    ObjectHeader objectHeader;
  };

  compat::Expected<ParseResultAndLength<SubgroupHeaderResult>, ErrorCode>
  parseSubgroupHeader(
      Cursor& cursor,
      size_t length,
      const SubgroupOptions& options) const noexcept;

  compat::Expected<ParseResultAndLength<ObjectHeader>, ErrorCode>
  parseFetchObjectHeader(
      Cursor& cursor,
      size_t length,
      const ObjectHeader& headerTemplate) const noexcept;

  compat::Expected<ParseResultAndLength<ObjectHeader>, ErrorCode>
  parseSubgroupObjectHeader(
      Cursor& cursor,
      size_t length,
      const ObjectHeader& headerTemplate,
      const SubgroupOptions& options) const noexcept;

  compat::Expected<SubscribeRequest, ErrorCode> parseSubscribeRequest(
      Cursor& cursor,
      size_t length) const noexcept;

  compat::Expected<SubscribeUpdate, ErrorCode> parseSubscribeUpdate(
      Cursor& cursor,
      size_t length) const noexcept;

  compat::Expected<SubscribeOk, ErrorCode> parseSubscribeOk(
      Cursor& cursor,
      size_t length) const noexcept;

  compat::Expected<Unsubscribe, ErrorCode> parseUnsubscribe(
      Cursor& cursor,
      size_t length) const noexcept;

  compat::Expected<SubscribeDone, ErrorCode> parseSubscribeDone(
      Cursor& cursor,
      size_t length) const noexcept;

  compat::Expected<PublishRequest, ErrorCode> parsePublish(
      Cursor& cursor,
      size_t length) const noexcept;

  compat::Expected<PublishOk, ErrorCode> parsePublishOk(
      Cursor& cursor,
      size_t length) const noexcept;

  compat::Expected<PublishNamespace, ErrorCode> parsePublishNamespace(
      Cursor& cursor,
      size_t length) const noexcept;

  compat::Expected<PublishNamespaceOk, ErrorCode> parsePublishNamespaceOk(
      Cursor& cursor,
      size_t length) const noexcept;

  compat::Expected<RequestOk, ErrorCode> parseRequestOk(
      Cursor& cursor,
      size_t length,
      FrameType frameType) const noexcept;

  compat::Expected<PublishNamespaceDone, ErrorCode> parsePublishNamespaceDone(
      Cursor& cursor,
      size_t length) const noexcept;

  compat::Expected<PublishNamespaceCancel, ErrorCode>
  parsePublishNamespaceCancel(Cursor& cursor, size_t length)
      const noexcept;

  compat::Expected<TrackStatus, ErrorCode> parseTrackStatus(
      Cursor& cursor,
      size_t length) const noexcept;

  compat::Expected<TrackStatusOk, ErrorCode> parseTrackStatusOk(
      Cursor& cursor,
      size_t length) const noexcept;

  compat::Expected<TrackStatusError, ErrorCode> parseTrackStatusError(
      Cursor& cursor,
      size_t length) const noexcept;

  compat::Expected<Goaway, ErrorCode> parseGoaway(
      Cursor& cursor,
      size_t length) const noexcept;

  compat::Expected<MaxRequestID, ErrorCode> parseMaxRequestID(
      Cursor& cursor,
      size_t length) const noexcept;

  compat::Expected<RequestsBlocked, ErrorCode> parseRequestsBlocked(
      Cursor& cursor,
      size_t length) const noexcept;

  compat::Expected<Fetch, ErrorCode> parseFetch(
      Cursor& cursor,
      size_t length) const noexcept;

  compat::Expected<FetchCancel, ErrorCode> parseFetchCancel(
      Cursor& cursor,
      size_t length) const noexcept;

  compat::Expected<FetchOk, ErrorCode> parseFetchOk(
      Cursor& cursor,
      size_t length) const noexcept;

  compat::Expected<SubscribeNamespace, ErrorCode> parseSubscribeNamespace(
      Cursor& cursor,
      size_t length) const noexcept;

  compat::Expected<SubscribeNamespaceOk, ErrorCode> parseSubscribeNamespaceOk(
      Cursor& cursor,
      size_t length) const noexcept;

  // Unified request error parsing function
  compat::Expected<RequestError, ErrorCode> parseRequestError(
      Cursor& cursor,
      size_t length,
      FrameType frameType) const noexcept;

  compat::Expected<UnsubscribeNamespace, ErrorCode> parseUnsubscribeNamespace(
      Cursor& cursor,
      size_t length) const noexcept;

  // v16+ messages for SUBSCRIBE_NAMESPACE response stream
  compat::Expected<Namespace, ErrorCode> parseNamespace(
      Cursor& cursor,
      size_t length) const noexcept;

  compat::Expected<NamespaceDone, ErrorCode> parseNamespaceDone(
      Cursor& cursor,
      size_t length) const noexcept;

  compat::Expected<compat::Unit, ErrorCode> parseExtensions(
      Cursor& cursor,
      size_t& length,
      ObjectHeader& objectHeader) const noexcept;

  void initializeVersion(uint64_t versionIn) {
    CHECK(!version_) << "Version already initialized";
    version_ = versionIn;
  }

  std::optional<uint64_t> getVersion() const {
    return version_;
  }

  void setTokenCacheMaxSize(size_t size) {
    tokenCache_.setMaxSize(size, /*evict=*/true);
  }

  // Test only
  void reset() {
    previousObjectID_ = std::nullopt;
    previousFetchGroup_ = std::nullopt;
    previousFetchSubgroup_ = std::nullopt;
    previousFetchPriority_ = std::nullopt;
  }

 private:
  // Legacy FETCH object parser (draft <= 14)
  compat::Expected<ObjectHeader, ErrorCode> parseFetchObjectHeaderLegacy(
      Cursor& cursor,
      size_t& length,
      const ObjectHeader& headerTemplate) const noexcept;

  // Draft-15+ FETCH object parser with Serialization Flags
  compat::Expected<ObjectHeader, ErrorCode> parseFetchObjectDraft15(
      Cursor& cursor,
      size_t& length,
      const ObjectHeader& headerTemplate) const noexcept;

  // Reset fetch context at start of new FETCH stream
  void resetFetchContext() const noexcept;

  compat::Expected<compat::Unit, ErrorCode> parseObjectStatusAndLength(
      Cursor& cursor,
      size_t& length,
      ObjectHeader& objectHeader) const noexcept;

  bool isValidStatusForExtensions(
      const ObjectHeader& objectHeader) const noexcept;

  compat::Expected<compat::Unit, ErrorCode> parseTrackRequestParams(
      Cursor& cursor,
      size_t& length,
      size_t numParams,
      TrackRequestParameters& params,
      std::vector<Parameter>& requestSpecificParams) const noexcept;

  compat::Expected<std::optional<AuthToken>, ErrorCode> parseToken(
      Cursor& cursor,
      size_t length) const noexcept;

  compat::Expected<std::vector<std::string>, ErrorCode> parseFixedTuple(
      Cursor& cursor,
      size_t& length) const noexcept;

  compat::Expected<FullTrackName, ErrorCode> parseFullTrackName(
      Cursor& cursor,
      size_t& length) const noexcept;

  compat::Expected<compat::Unit, ErrorCode> parseExtensionKvPairs(
      Cursor& cursor,
      ObjectHeader& objectHeader,
      size_t extensionBlockLength,
      bool allowImmutable = true) const noexcept;

  compat::Expected<compat::Unit, ErrorCode> parseExtension(
      Cursor& cursor,
      size_t& length,
      ObjectHeader& objectHeader,
      bool allowImmutable = true) const noexcept;

  std::optional<SubscriptionFilter> extractSubscriptionFilter(
      const std::vector<Parameter>& requestSpecificParams) const noexcept;

  void handleRequestSpecificParams(
      SubscribeRequest& subscribeRequest,
      const std::vector<Parameter>& requestSpecificParams) const noexcept;

  void handleRequestSpecificParams(
      SubscribeOk& subscribeOk,
      const std::vector<Parameter>& requestSpecificParams) const noexcept;

  void handleRequestSpecificParams(
      SubscribeUpdate& subscribeUpdate,
      const std::vector<Parameter>& requestSpecificParams) const noexcept;

  void handleRequestSpecificParams(
      PublishRequest& publishRequest,
      const std::vector<Parameter>& requestSpecificParams) const noexcept;

  void handleRequestSpecificParams(
      PublishOk& publishOk,
      const std::vector<Parameter>& requestSpecificParams) const noexcept;

  void handleRequestSpecificParams(
      Fetch& fetchRequest,
      const std::vector<Parameter>& requestSpecificParams) const noexcept;

  void handleGroupOrderParam(
      GroupOrder& groupOrderField,
      const std::vector<Parameter>& requestSpecificParams,
      GroupOrder defaultGroupOrder) const noexcept;

  void handleSubscriberPriorityParam(
      uint8_t& priorityField,
      const std::vector<Parameter>& requestSpecificParams) const noexcept;

  void handleForwardParam(
      bool& forwardField,
      const std::vector<Parameter>& requestSpecificParams) const noexcept;

  // Overload for Optional<bool> - used by SubscribeUpdate
  void handleForwardParam(
      std::optional<bool>& forwardField,
      const std::vector<Parameter>& requestSpecificParams) const noexcept;

  std::optional<uint64_t> version_;
  mutable MoQTokenCache tokenCache_;
  mutable std::optional<uint64_t> previousObjectID_;
  // Context for FETCH object delta encoding (draft-15+)
  mutable std::optional<uint64_t> previousFetchGroup_;
  mutable std::optional<uint64_t> previousFetchSubgroup_;
  mutable std::optional<uint8_t> previousFetchPriority_;
  // Context for extension delta decoding (draft-16+)
  mutable uint64_t previousExtensionType_ = 0;
};

//// Egress ////
TrackRequestParameter getAuthParam(
    uint64_t version,
    std::string token,
    uint64_t tokenType = 0,
    std::optional<uint64_t> registerToken = AuthToken::Register);

WriteResult writeClientSetup(
    BufQueue& writeBuf,
    const ClientSetup& clientSetup,
    uint64_t version) noexcept;

WriteResult writeServerSetup(
    BufQueue& writeBuf,
    const ServerSetup& serverSetup,
    uint64_t version) noexcept;

// writeClientSetup and writeServerSetup are the only two functions that
// are version-agnostic, so we are leaving them out of the MoQFrameWriter.
class MoQFrameWriter {
 public:
  WriteResult writeSubgroupHeader(
      BufQueue& writeBuf,
      TrackAlias trackAlias,
      const ObjectHeader& objectHeader,
      SubgroupIDFormat format = SubgroupIDFormat::Present,
      bool includeExtensions = true) const noexcept;

  WriteResult writeFetchHeader(BufQueue& writeBuf, RequestID requestID)
      const noexcept;

  WriteResult writeStreamHeader(
      BufQueue& writeBuf,
      StreamType streamType,
      TrackAlias trackAlias,
      const ObjectHeader& objectHeader) const noexcept;

  WriteResult writeDatagramObject(
      BufQueue& writeBuf,
      TrackAlias trackAlias,
      const ObjectHeader& objectHeader,
      std::unique_ptr<Buffer> objectPayload) const noexcept;

  WriteResult writeStreamObject(
      BufQueue& writeBuf,
      StreamType streamType,
      const ObjectHeader& objectHeader,
      std::unique_ptr<Buffer> objectPayload) const noexcept;

  WriteResult writeSingleObjectStream(
      BufQueue& writeBuf,
      TrackAlias trackAlias,
      const ObjectHeader& objectHeader,
      std::unique_ptr<Buffer> objectPayload) const noexcept;

  WriteResult writeSubscribeRequest(
      BufQueue& writeBuf,
      const SubscribeRequest& subscribeRequest) const noexcept;

  WriteResult writeSubscribeUpdate(
      BufQueue& writeBuf,
      const SubscribeUpdate& update) const noexcept;

  WriteResult writeSubscribeOk(
      BufQueue& writeBuf,
      const SubscribeOk& subscribeOk) const noexcept;

  WriteResult writeSubscribeDone(
      BufQueue& writeBuf,
      const SubscribeDone& subscribeDone) const noexcept;

  WriteResult writeUnsubscribe(
      BufQueue& writeBuf,
      const Unsubscribe& unsubscribe) const noexcept;

  WriteResult writePublish(
      BufQueue& writeBuf,
      const PublishRequest& publish) const noexcept;

  WriteResult writePublishOk(
      BufQueue& writeBuf,
      const PublishOk& publishOk) const noexcept;

  WriteResult writeMaxRequestID(
      BufQueue& writeBuf,
      const MaxRequestID& maxRequestID) const noexcept;

  WriteResult writeRequestsBlocked(
      BufQueue& writeBuf,
      const RequestsBlocked& subscribesBlocked) const noexcept;

  WriteResult writePublishNamespace(
      BufQueue& writeBuf,
      const PublishNamespace& publishNamespace) const noexcept;

  WriteResult writePublishNamespaceOk(
      BufQueue& writeBuf,
      const PublishNamespaceOk& publishNamespaceOk) const noexcept;

  WriteResult writeRequestOk(
      BufQueue& writeBuf,
      const RequestOk& requestOk,
      FrameType frameType) const noexcept;

  WriteResult writePublishNamespaceDone(
      BufQueue& writeBuf,
      const PublishNamespaceDone& publishNamespaceDone) const noexcept;

  WriteResult writePublishNamespaceCancel(
      BufQueue& writeBuf,
      const PublishNamespaceCancel& publishNamespaceCancel) const noexcept;

  WriteResult writeTrackStatus(
      BufQueue& writeBuf,
      const TrackStatus& trackStatus) const noexcept;

  WriteResult writeTrackStatusOk(
      BufQueue& writeBuf,
      const TrackStatusOk& trackStatusOk) const noexcept;

  WriteResult writeTrackStatusError(
      BufQueue& writeBuf,
      const TrackStatusError& trackStatusError) const noexcept;

  WriteResult writeGoaway(BufQueue& writeBuf, const Goaway& goaway)
      const noexcept;

  WriteResult writeSubscribeNamespace(
      BufQueue& writeBuf,
      const SubscribeNamespace& subscribeNamespace) const noexcept;

  WriteResult writeSubscribeNamespaceOk(
      BufQueue& writeBuf,
      const SubscribeNamespaceOk& subscribeNamespaceOk) const noexcept;

  WriteResult writeUnsubscribeNamespace(
      BufQueue& writeBuf,
      const UnsubscribeNamespace& unsubscribeNamespace) const noexcept;

  // v16+ messages for SUBSCRIBE_NAMESPACE response stream
  WriteResult writeNamespace(BufQueue& writeBuf, const Namespace& ns)
      const noexcept;

  WriteResult writeNamespaceDone(
      BufQueue& writeBuf,
      const NamespaceDone& namespaceDone) const noexcept;

  WriteResult writeFetch(BufQueue& writeBuf, const Fetch& fetch)
      const noexcept;

  WriteResult writeFetchCancel(
      BufQueue& writeBuf,
      const FetchCancel& fetchCancel) const noexcept;

  WriteResult writeFetchOk(BufQueue& writeBuf, const FetchOk& fetchOk)
      const noexcept;

  // Unified request error writing function
  WriteResult writeRequestError(
      BufQueue& writeBuf,
      const RequestError& requestError,
      FrameType frameType) const noexcept;

  std::string encodeUseAlias(uint64_t alias) const;

  std::string encodeDeleteTokenAlias(uint64_t alias) const;

  std::string encodeRegisterToken(
      uint64_t alias,
      uint64_t tokenType,
      const std::string& tokenValue) const;

  std::string encodeTokenValue(
      uint64_t tokenType,
      const std::string& tokenValue,
      const std::optional<uint64_t>& forceVersion = std::nullopt) const;

  void initializeVersion(uint64_t versionIn) {
    CHECK(!version_) << "Version already initialized";
    version_ = versionIn;
  }

  std::optional<uint64_t> getVersion() const {
    return version_;
  }

  void writeExtensions(
      BufQueue& writeBuf,
      const Extensions& extensions,
      size_t& size,
      bool& error) const noexcept;

 private:
  void writeKeyValuePairs(
      BufQueue& writeBuf,
      const std::vector<Extension>& extensions,
      size_t& size,
      bool& error) const noexcept;

  size_t calculateExtensionVectorSize(
      const std::vector<Extension>& extensions,
      bool& error) const noexcept;

  void writeTrackRequestParams(
      BufQueue& writeBuf,
      const TrackRequestParameters& params,
      const std::vector<Parameter>& requestSpecificParams,
      size_t& size,
      bool& error) const noexcept;

  void writeParamValue(
      BufQueue& writeBuf,
      const Parameter& param,
      size_t& size,
      bool& error) const noexcept;

  void writeSubscriptionFilter(
      BufQueue& writeBuf,
      const SubscriptionFilter& filter,
      size_t& size,
      bool& error) const noexcept;

  WriteResult writeSubscribeOkHelper(
      BufQueue& writeBuf,
      const SubscribeOk& subscribeOk) const noexcept;

  WriteResult writeSubscribeRequestHelper(
      BufQueue& writeBuf,
      const SubscribeRequest& subscribeRequest) const noexcept;

  // Legacy FETCH object writer (draft <= 14)
  void writeFetchObjectHeaderLegacy(
      BufQueue& writeBuf,
      const ObjectHeader& objectHeader,
      size_t& size,
      bool& error) const noexcept;

  // Draft-15+ FETCH object writer with Serialization Flags
  void writeFetchObjectDraft15(
      BufQueue& writeBuf,
      const ObjectHeader& objectHeader,
      size_t& size,
      bool& error) const noexcept;

  void resetWriterFetchContext() const noexcept;

  std::optional<uint64_t> version_;
  mutable std::optional<uint64_t> previousObjectID_;
  // Context for FETCH object delta encoding (draft-15+)
  mutable std::optional<uint64_t> previousFetchGroup_;
  mutable std::optional<uint64_t> previousFetchSubgroup_;
  mutable std::optional<uint8_t> previousFetchPriority_;
};

} // namespace moxygen
