/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// Std-mode framer tests - works without Folly dependencies
// This file tests MoQFramer parsing and serialization in std-mode

#include <moxygen/MoQFramer.h>
#include <moxygen/MoQTypes.h>
#include <moxygen/MoQVersions.h>
#include <moxygen/compat/BufferAppender.h>
#include <moxygen/compat/BufferIO.h>
#include <moxygen/compat/Varint.h>

#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace moxygen::test {

// Helper to create a buffer from a string
inline std::unique_ptr<Buffer> makeBuffer(const std::string& str) {
  return Buffer::copyBuffer(str.data(), str.size());
}

// Helper to create a buffer with specific bytes
inline std::unique_ptr<Buffer> makeBuffer(std::initializer_list<uint8_t> bytes) {
  std::vector<uint8_t> data(bytes);
  return Buffer::copyBuffer(data.data(), data.size());
}

// Helper to write a QUIC varint to a buffer queue
inline void writeVarintTo(BufQueue& q, uint64_t value) {
  size_t size = 0;
  bool error = false;
  writeVarint(q, value, size, error);
  ASSERT_FALSE(error) << "writeVarint failed for value " << value;
}

// Helper to skip frame header and get payload length
// Frame format: varint(frameType) + uint16_be(length) + payload
size_t skipFrameHeader(Cursor& cursor) {
  // Skip frame type varint
  auto frameTypeResult = quic::follyutils::decodeQuicInteger(cursor);
  EXPECT_TRUE(frameTypeResult.has_value());

  // Read 2-byte big-endian frame length
  if (!cursor.canAdvance(2)) {
    ADD_FAILURE() << "Not enough bytes for frame length";
    return 0;
  }
  uint16_t len = cursor.readBE<uint16_t>();
  return len;
}

// Test fixture for std-mode framer tests
class MoQFramerTestStd : public ::testing::TestWithParam<uint64_t> {
 public:
  void SetUp() override {
    version_ = GetParam();
    parser_.initializeVersion(version_);
    writer_.initializeVersion(version_);
  }

 protected:
  uint64_t version_;
  MoQFrameParser parser_;
  MoQFrameWriter writer_;
};

// Test basic varint encoding/decoding
TEST_P(MoQFramerTestStd, VarintRoundTrip) {
  std::vector<uint64_t> testValues = {
      0, 1, 63,           // 1-byte
      64, 16383,          // 2-byte
      16384, 1073741823,  // 4-byte
      1073741824, (1ULL << 62) - 1  // 8-byte
  };

  for (uint64_t value : testValues) {
    BufQueue writeBuf;
    writeVarintTo(writeBuf, value);

    auto buf = writeBuf.move();
    ASSERT_NE(buf, nullptr);

    Cursor cursor(buf.get());
    auto decoded = quic::follyutils::decodeQuicInteger(cursor);
    ASSERT_TRUE(decoded.has_value()) << "Failed to decode varint for " << value;
    EXPECT_EQ(decoded->first, value);
  }
}

// Test ClientSetup serialization and parsing
TEST_P(MoQFramerTestStd, ClientSetupRoundTrip) {
  ClientSetup clientSetup;
  clientSetup.supportedVersions = {version_};
  clientSetup.params.insertParam(
      Parameter(compat::to_underlying(SetupKey::PATH), "/test"));
  clientSetup.params.insertParam(
      Parameter(compat::to_underlying(SetupKey::MAX_REQUEST_ID), 100));

  BufQueue writeBuf;
  auto res = writeClientSetup(writeBuf, clientSetup, version_);
  ASSERT_TRUE(res.hasValue()) << "writeClientSetup failed";

  auto buf = writeBuf.move();
  ASSERT_NE(buf, nullptr);

  Cursor cursor(buf.get());
  size_t frameLen = skipFrameHeader(cursor);
  ASSERT_GT(frameLen, 0);

  auto parsedSetup = parser_.parseClientSetup(cursor, frameLen);
  ASSERT_TRUE(parsedSetup.hasValue()) << "parseClientSetup failed";

  // For Draft15+, versions are negotiated via ALPN, not in the frame
  if (getDraftMajorVersion(version_) < 15) {
    EXPECT_EQ(parsedSetup->supportedVersions.size(), 1);
    EXPECT_EQ(parsedSetup->supportedVersions[0], version_);
  } else {
    // Versions not included in ALPN mode
    EXPECT_EQ(parsedSetup->supportedVersions.size(), 0);
  }
}

// Test ServerSetup serialization and parsing
TEST_P(MoQFramerTestStd, ServerSetupRoundTrip) {
  ServerSetup serverSetup;
  serverSetup.selectedVersion = version_;
  serverSetup.params.insertParam(
      Parameter(compat::to_underlying(SetupKey::PATH), "/test"));

  BufQueue writeBuf;
  auto res = writeServerSetup(writeBuf, serverSetup, version_);
  ASSERT_TRUE(res.hasValue()) << "writeServerSetup failed";

  auto buf = writeBuf.move();
  ASSERT_NE(buf, nullptr);

  Cursor cursor(buf.get());
  size_t frameLen = skipFrameHeader(cursor);
  ASSERT_GT(frameLen, 0);

  auto parsedSetup = parser_.parseServerSetup(cursor, frameLen);
  ASSERT_TRUE(parsedSetup.hasValue()) << "parseServerSetup failed";

  EXPECT_EQ(parsedSetup->selectedVersion, version_);
}

// Test SubscribeRequest serialization and parsing
TEST_P(MoQFramerTestStd, SubscribeRequestRoundTrip) {
  auto req = SubscribeRequest::make(
      FullTrackName({TrackNamespace({"ns1", "ns2"}), "trackname"}),
      128,  // priority
      GroupOrder::OldestFirst,
      true,  // forward
      LocationType::LargestObject,
      std::nullopt,  // start
      0,  // endGroup
      {});  // params

  BufQueue writeBuf;
  auto res = writer_.writeSubscribeRequest(writeBuf, req);
  ASSERT_TRUE(res.hasValue()) << "writeSubscribeRequest failed";

  auto buf = writeBuf.move();
  ASSERT_NE(buf, nullptr);

  Cursor cursor(buf.get());
  size_t frameLen = skipFrameHeader(cursor);
  ASSERT_GT(frameLen, 0);

  auto parsed = parser_.parseSubscribeRequest(cursor, frameLen);
  ASSERT_TRUE(parsed.hasValue()) << "parseSubscribeRequest failed";

  EXPECT_EQ(parsed->fullTrackName.trackNamespace.size(), 2);
  EXPECT_EQ(parsed->fullTrackName.trackName, "trackname");
  EXPECT_EQ(parsed->priority, 128);
  EXPECT_EQ(parsed->groupOrder, GroupOrder::OldestFirst);
}

// Test SubscribeOk serialization and parsing
TEST_P(MoQFramerTestStd, SubscribeOkRoundTrip) {
  SubscribeOk subOk;
  subOk.requestID = RequestID(42);
  subOk.trackAlias = TrackAlias(17);
  subOk.expires = std::chrono::milliseconds(5000);
  subOk.groupOrder = GroupOrder::NewestFirst;
  subOk.largest = AbsoluteLocation{10, 5};

  BufQueue writeBuf;
  auto res = writer_.writeSubscribeOk(writeBuf, subOk);
  ASSERT_TRUE(res.hasValue()) << "writeSubscribeOk failed";

  auto buf = writeBuf.move();
  ASSERT_NE(buf, nullptr);

  Cursor cursor(buf.get());
  size_t frameLen = skipFrameHeader(cursor);
  ASSERT_GT(frameLen, 0);

  auto parsed = parser_.parseSubscribeOk(cursor, frameLen);
  ASSERT_TRUE(parsed.hasValue()) << "parseSubscribeOk failed";

  EXPECT_EQ(parsed->requestID, RequestID(42));
  EXPECT_EQ(parsed->trackAlias, TrackAlias(17));
  EXPECT_EQ(parsed->groupOrder, GroupOrder::NewestFirst);
}

// Test RequestError serialization and parsing
TEST_P(MoQFramerTestStd, RequestErrorRoundTrip) {
  RequestError reqErr;
  reqErr.requestID = RequestID(99);
  reqErr.errorCode = RequestErrorCode::TRACK_NOT_EXIST;
  reqErr.reasonPhrase = "Track not found";

  BufQueue writeBuf;
  auto res = writer_.writeRequestError(writeBuf, reqErr, FrameType::SUBSCRIBE_ERROR);
  ASSERT_TRUE(res.hasValue()) << "writeRequestError failed";

  auto buf = writeBuf.move();
  ASSERT_NE(buf, nullptr);

  Cursor cursor(buf.get());
  size_t frameLen = skipFrameHeader(cursor);
  ASSERT_GT(frameLen, 0);

  auto parsed = parser_.parseRequestError(cursor, frameLen, FrameType::SUBSCRIBE_ERROR);
  ASSERT_TRUE(parsed.hasValue()) << "parseRequestError failed";

  EXPECT_EQ(parsed->requestID, RequestID(99));
  EXPECT_EQ(parsed->errorCode, RequestErrorCode::TRACK_NOT_EXIST);
  EXPECT_EQ(parsed->reasonPhrase, "Track not found");
}

// Test Unsubscribe serialization and parsing
TEST_P(MoQFramerTestStd, UnsubscribeRoundTrip) {
  Unsubscribe unsub;
  unsub.requestID = RequestID(123);

  BufQueue writeBuf;
  auto res = writer_.writeUnsubscribe(writeBuf, unsub);
  ASSERT_TRUE(res.hasValue()) << "writeUnsubscribe failed";

  auto buf = writeBuf.move();
  ASSERT_NE(buf, nullptr);

  Cursor cursor(buf.get());
  size_t frameLen = skipFrameHeader(cursor);
  ASSERT_GT(frameLen, 0);

  auto parsed = parser_.parseUnsubscribe(cursor, frameLen);
  ASSERT_TRUE(parsed.hasValue()) << "parseUnsubscribe failed";

  EXPECT_EQ(parsed->requestID, RequestID(123));
}

// Test Fetch serialization and parsing
TEST_P(MoQFramerTestStd, FetchRoundTrip) {
  Fetch fetch;
  fetch.requestID = RequestID(7);
  fetch.fullTrackName = FullTrackName({TrackNamespace({"live"}), "video"});
  fetch.args = StandaloneFetch(
      AbsoluteLocation({0, 0}),
      AbsoluteLocation({10, 100}));
  fetch.priority = 200;
  fetch.groupOrder = GroupOrder::NewestFirst;

  BufQueue writeBuf;
  auto res = writer_.writeFetch(writeBuf, fetch);
  ASSERT_TRUE(res.hasValue()) << "writeFetch failed";

  auto buf = writeBuf.move();
  ASSERT_NE(buf, nullptr);

  Cursor cursor(buf.get());
  size_t frameLen = skipFrameHeader(cursor);
  ASSERT_GT(frameLen, 0);

  auto parsed = parser_.parseFetch(cursor, frameLen);
  ASSERT_TRUE(parsed.hasValue()) << "parseFetch failed";

  EXPECT_EQ(parsed->requestID, RequestID(7));
  EXPECT_EQ(parsed->fullTrackName.trackName, "video");
  EXPECT_EQ(parsed->priority, 200);
}

// Test FetchOk serialization and parsing
TEST_P(MoQFramerTestStd, FetchOkRoundTrip) {
  FetchOk fetchOk;
  fetchOk.requestID = RequestID(7);
  fetchOk.groupOrder = GroupOrder::OldestFirst;
  fetchOk.endOfTrack = 0;
  fetchOk.endLocation = AbsoluteLocation({5, 25});

  BufQueue writeBuf;
  auto res = writer_.writeFetchOk(writeBuf, fetchOk);
  ASSERT_TRUE(res.hasValue()) << "writeFetchOk failed";

  auto buf = writeBuf.move();
  ASSERT_NE(buf, nullptr);

  Cursor cursor(buf.get());
  size_t frameLen = skipFrameHeader(cursor);
  ASSERT_GT(frameLen, 0);

  auto parsed = parser_.parseFetchOk(cursor, frameLen);
  ASSERT_TRUE(parsed.hasValue()) << "parseFetchOk failed";

  EXPECT_EQ(parsed->requestID, RequestID(7));
  EXPECT_EQ(parsed->groupOrder, GroupOrder::OldestFirst);
}

// Test Goaway serialization and parsing
TEST_P(MoQFramerTestStd, GoawayRoundTrip) {
  Goaway goaway;
  goaway.newSessionUri = "https://new.server.example/moq";

  BufQueue writeBuf;
  auto res = writer_.writeGoaway(writeBuf, goaway);
  ASSERT_TRUE(res.hasValue()) << "writeGoaway failed";

  auto buf = writeBuf.move();
  ASSERT_NE(buf, nullptr);

  Cursor cursor(buf.get());
  size_t frameLen = skipFrameHeader(cursor);
  ASSERT_GT(frameLen, 0);

  auto parsed = parser_.parseGoaway(cursor, frameLen);
  ASSERT_TRUE(parsed.hasValue()) << "parseGoaway failed";

  EXPECT_EQ(parsed->newSessionUri, "https://new.server.example/moq");
}

// Test MaxRequestID serialization and parsing
TEST_P(MoQFramerTestStd, MaxRequestIDRoundTrip) {
  MaxRequestID maxReqId;
  maxReqId.requestID = RequestID(50000);

  BufQueue writeBuf;
  auto res = writer_.writeMaxRequestID(writeBuf, maxReqId);
  ASSERT_TRUE(res.hasValue()) << "writeMaxRequestID failed";

  auto buf = writeBuf.move();
  ASSERT_NE(buf, nullptr);

  Cursor cursor(buf.get());
  size_t frameLen = skipFrameHeader(cursor);
  ASSERT_GT(frameLen, 0);

  auto parsed = parser_.parseMaxRequestID(cursor, frameLen);
  ASSERT_TRUE(parsed.hasValue()) << "parseMaxRequestID failed";

  EXPECT_EQ(parsed->requestID, RequestID(50000));
}

// Test PublishDone serialization and parsing
TEST_P(MoQFramerTestStd, PublishDoneRoundTrip) {
  PublishDone pubDone;
  pubDone.requestID = RequestID(88);
  pubDone.statusCode = PublishDoneStatusCode::SUBSCRIPTION_ENDED;
  pubDone.reasonPhrase = "Normal termination";

  BufQueue writeBuf;
  auto res = writer_.writePublishDone(writeBuf, pubDone);
  ASSERT_TRUE(res.hasValue()) << "writePublishDone failed";

  auto buf = writeBuf.move();
  ASSERT_NE(buf, nullptr);

  Cursor cursor(buf.get());
  size_t frameLen = skipFrameHeader(cursor);
  ASSERT_GT(frameLen, 0);

  auto parsed = parser_.parsePublishDone(cursor, frameLen);
  ASSERT_TRUE(parsed.hasValue()) << "parsePublishDone failed";

  EXPECT_EQ(parsed->requestID, RequestID(88));
  EXPECT_EQ(parsed->statusCode, PublishDoneStatusCode::SUBSCRIPTION_ENDED);
}

// Test ObjectHeader serialization and parsing
TEST_P(MoQFramerTestStd, ObjectHeaderRoundTrip) {
  ObjectHeader objHeader(
      /*group=*/5,
      /*subgroup=*/2,
      /*id=*/100,
      /*priority=*/64,
      /*length=*/1024);

  BufQueue writeBuf;
  auto res = writer_.writeSubgroupHeader(
      writeBuf,
      TrackAlias(1),
      objHeader,
      SubgroupIDFormat::Present,
      /*priorityPresent=*/true);
  ASSERT_TRUE(res.hasValue()) << "writeSubgroupHeader failed";

  auto buf = writeBuf.move();
  ASSERT_NE(buf, nullptr);

  Cursor cursor(buf.get());
  size_t length = buf->computeChainDataLength();

  // Parse the stream type varint first
  auto streamTypeDecoded = quic::follyutils::decodeQuicInteger(cursor);
  ASSERT_TRUE(streamTypeDecoded.has_value()) << "parseStreamType failed";
  length -= streamTypeDecoded->second;

  SubgroupOptions options;
  options.subgroupIDFormat = SubgroupIDFormat::Present;
  options.priorityPresent = true;

  auto parsed = parser_.parseSubgroupHeader(cursor, length, options);
  ASSERT_TRUE(parsed.hasValue()) << "parseSubgroupHeader failed";

  EXPECT_EQ(parsed->value.trackAlias, TrackAlias(1));
  EXPECT_EQ(parsed->value.objectHeader.group, 5);
  EXPECT_EQ(parsed->value.objectHeader.subgroup, 2);
}

// Test empty buffer handling
TEST_P(MoQFramerTestStd, EmptyBufferParsing) {
  BufQueue emptyBuf;
  auto buf = emptyBuf.move();

  // Parsing an empty buffer should return nullopt, not crash
  if (buf) {
    Cursor cursor(buf.get());
    auto result = parser_.parseClientSetup(cursor, 0);
    EXPECT_FALSE(result.hasValue());
  }
}

// Test truncated frame handling
TEST_P(MoQFramerTestStd, TruncatedFrameParsing) {
  // Create a valid ClientSetup
  ClientSetup clientSetup;
  clientSetup.supportedVersions = {version_};

  BufQueue writeBuf;
  auto res = writeClientSetup(writeBuf, clientSetup, version_);
  ASSERT_TRUE(res.hasValue());

  auto fullBuf = writeBuf.move();
  ASSERT_NE(fullBuf, nullptr);

  // Truncate the buffer to half its size
  size_t halfSize = fullBuf->computeChainDataLength() / 2;
  if (halfSize > 0) {
    auto truncatedBuf = Buffer::copyBuffer(fullBuf->data(), halfSize);

    Cursor cursor(truncatedBuf.get());
    auto result = parser_.parseClientSetup(cursor, halfSize);
    // Should fail gracefully with underflow
    EXPECT_FALSE(result.hasValue());
  }
}

// Instantiate tests for current draft version
// Note: Draft15+ have different frame formats that would need separate test cases
INSTANTIATE_TEST_SUITE_P(
    MoQFramerTestStd,
    MoQFramerTestStd,
    ::testing::Values(kVersionDraftCurrent));

} // namespace moxygen::test

// Test main for std-mode
int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
