/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "moxygen/openmoq/transport/pico/PicoWebTransportBase.h"

#include <folly/io/IOBuf.h>
#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>
#include <picoquic.h>
#include <picoquic_utils.h>

using namespace testing;

namespace moxygen::test {

namespace {

/**
 * Records what the transport hands to picoquic rather than calling into it, so
 * the JIT send path can be driven a pull at a time with no packet in flight.
 */
class TestPicoWebTransport : public PicoWebTransportBase {
 public:
  explicit TestPicoWebTransport(picoquic_cnx_t* cnx)
      : PicoWebTransportBase(
            cnx,
            /*isClient=*/true,
            folly::SocketAddress("127.0.0.1", 1234),
            folly::SocketAddress("127.0.0.1", 4433)) {}

  struct StreamWrite {
    size_t length{0};
    bool fin{false};
    bool isStillActive{false};
  };

  // One picoquic prepare_to_send. picoquic keeps pulling while the last call
  // said still-active, so a stream drains in two pulls: data, then empty.
  bool pull(uint64_t streamId, size_t maxLength = 1024) {
    return onJitProvideData(streamId, /*picoContext=*/nullptr, maxLength);
  }

  std::string lastPayload() const {
    return writes.empty() ? std::string()
                          : std::string(
                                reinterpret_cast<const char*>(scratch_.data()),
                                writes.back().length);
  }

  std::vector<uint64_t> activated;
  std::vector<std::pair<uint64_t, uint32_t>> resets;
  std::vector<std::pair<uint64_t, uint32_t>> stopSendings;
  std::vector<StreamWrite> writes;

 protected:
  folly::Expected<uint64_t, ErrorCode> createStreamImpl(bool bidi) override {
    if (bidi) {
      return folly::makeUnexpected(ErrorCode::STREAM_CREATION_ERROR);
    }
    auto id = nextUniStream_;
    nextUniStream_ += 4;
    return id;
  }

  void markStreamActiveImpl(uint64_t streamId) override {
    activated.push_back(streamId);
  }

  void markDatagramActiveImpl() override {}

  void resetStreamImpl(uint64_t streamId, uint32_t error) override {
    resets.emplace_back(streamId, error);
  }

  void stopSendingImpl(uint64_t streamId, uint32_t error) override {
    stopSendings.emplace_back(streamId, error);
  }

  void sendCloseImpl(uint32_t /*errorCode*/) override {}

  uint8_t* getStreamDataBuffer(
      uint8_t* /*picoContext*/,
      size_t length,
      bool fin,
      bool isStillActive) override {
    writes.push_back({length, fin, isStillActive});
    return scratch_.data();
  }

  uint8_t* getDatagramBuffer(uint8_t*, size_t, bool) override {
    return nullptr;
  }

 private:
  // Client-initiated unidirectional stream ids.
  uint64_t nextUniStream_{2};
  std::array<uint8_t, 2048> scratch_{};
};

} // namespace

class PicoWebTransportBaseTest : public Test {
 protected:
  void SetUp() override {
    quic_ = picoquic_create(
        /*max_nb_connections=*/4,
        /*cert_file_name=*/nullptr,
        /*key_file_name=*/nullptr,
        /*cert_root_file_name=*/nullptr,
        /*default_alpn=*/"moq-00",
        /*default_callback_fn=*/nullptr,
        /*default_callback_ctx=*/nullptr,
        /*cnx_id_callback=*/nullptr,
        /*cnx_id_callback_data=*/nullptr,
        /*reset_seed=*/nullptr,
        picoquic_current_time(),
        /*p_simulated_time=*/nullptr,
        /*ticket_file_name=*/nullptr,
        /*ticket_encryption_key=*/nullptr,
        /*ticket_encryption_key_length=*/0);
    ASSERT_NE(quic_, nullptr);

    folly::SocketAddress peer("127.0.0.1", 4433);
    sockaddr_storage storage;
    peer.getAddress(&storage);
    cnx_ = picoquic_create_cnx(
        quic_,
        picoquic_null_connection_id,
        picoquic_null_connection_id,
        reinterpret_cast<const sockaddr*>(&storage),
        picoquic_current_time(),
        /*preferred_version=*/0,
        /*sni=*/"test",
        /*alpn=*/"moq-00",
        /*client_mode=*/1);
    ASSERT_NE(cnx_, nullptr);

    wt_ = std::make_unique<TestPicoWebTransport>(cnx_);
  }

  void TearDown() override {
    // The transport touches cnx_ as it shuts down, so it goes first.
    wt_.reset();
    if (quic_) {
      picoquic_free(quic_);
    }
  }

  proxygen::WebTransport::StreamWriteHandle* createUni() {
    auto handle = wt_->createUniStream();
    EXPECT_TRUE(handle.hasValue());
    return handle.hasValue() ? handle.value() : nullptr;
  }

  picoquic_quic_t* quic_{nullptr};
  picoquic_cnx_t* cnx_{nullptr};
  std::unique_ptr<TestPicoWebTransport> wt_;
};

TEST_F(PicoWebTransportBaseTest, WriteSchedulesStream) {
  auto* wh = createUni();
  ASSERT_NE(wh, nullptr);
  auto id = wh->getID();

  wh->writeStreamData(
      folly::IOBuf::copyBuffer("hello"), /*fin=*/false, nullptr);

  EXPECT_THAT(wt_->activated, ElementsAre(id));
}

TEST_F(PicoWebTransportBaseTest, JitProvideDataCopiesPayloadAndFin) {
  auto* wh = createUni();
  ASSERT_NE(wh, nullptr);
  auto id = wh->getID();
  wh->writeStreamData(folly::IOBuf::copyBuffer("hello"), /*fin=*/true, nullptr);

  EXPECT_TRUE(wt_->pull(id));

  ASSERT_THAT(wt_->writes, SizeIs(1));
  EXPECT_EQ(wt_->writes[0].length, 5);
  EXPECT_TRUE(wt_->writes[0].fin);
  EXPECT_FALSE(wt_->writes[0].isStillActive);
  EXPECT_EQ(wt_->lastPayload(), "hello");
}

TEST_F(PicoWebTransportBaseTest, DrainedStreamHandsOffToTheNextOne) {
  auto* first = createUni();
  ASSERT_NE(first, nullptr);
  auto firstId = first->getID();
  first->writeStreamData(folly::IOBuf::copyBuffer("one"), false, nullptr);

  auto* second = createUni();
  ASSERT_NE(second, nullptr);
  auto secondId = second->getID();
  second->writeStreamData(folly::IOBuf::copyBuffer("two"), false, nullptr);
  // Only the head of the queue is active; the second stream waits its turn.
  ASSERT_THAT(wt_->activated, ElementsAre(firstId));

  wt_->pull(firstId);
  wt_->pull(firstId);

  EXPECT_THAT(wt_->activated, ElementsAre(firstId, secondId));
}

// An event queued while another stream is still writable fires no callback, so
// unless the JIT path drains it the edge never re-arms again.
TEST_F(PicoWebTransportBaseTest, PriorityChangeMidBurstDoesNotStallEgress) {
  auto* group0 = createUni();
  ASSERT_NE(group0, nullptr);
  auto group0Id = group0->getID();
  group0->writeStreamData(folly::IOBuf::copyBuffer("group-0"), false, nullptr);
  ASSERT_THAT(wt_->activated, ElementsAre(group0Id));

  // Prioritized while group 0 still has data queued: the silent enqueue.
  auto* group1 = createUni();
  ASSERT_NE(group1, nullptr);
  auto group1Id = group1->getID();
  group1->setPriority(quic::HTTPPriorityQueue::Priority(1, false, 1));
  group1->writeStreamData(folly::IOBuf::copyBuffer("group-1"), false, nullptr);

  wt_->pull(group0Id);
  wt_->pull(group0Id);
  wt_->pull(group1Id);
  wt_->pull(group1Id);

  wt_->activated.clear();
  auto* group2 = createUni();
  ASSERT_NE(group2, nullptr);
  auto group2Id = group2->getID();
  group2->writeStreamData(folly::IOBuf::copyBuffer("group-2"), false, nullptr);

  EXPECT_THAT(wt_->activated, ElementsAre(group2Id));
}

// Resetting the active stream closes its handle, so picoquic's next pull for
// it finds nothing. The next queued stream must still get scheduled.
TEST_F(PicoWebTransportBaseTest, ResetOfActiveStreamStillSchedulesTheNextOne) {
  auto* first = createUni();
  ASSERT_NE(first, nullptr);
  auto firstId = first->getID();
  first->writeStreamData(folly::IOBuf::copyBuffer("one"), false, nullptr);

  auto* second = createUni();
  ASSERT_NE(second, nullptr);
  auto secondId = second->getID();
  second->writeStreamData(folly::IOBuf::copyBuffer("two"), false, nullptr);
  ASSERT_THAT(wt_->activated, ElementsAre(firstId));

  first->resetStream(42);

  wt_->pull(firstId);

  EXPECT_THAT(wt_->resets, ElementsAre(Pair(firstId, 42u)));
  EXPECT_THAT(wt_->activated, ElementsAre(firstId, secondId));
}

// Same latch, reached through an event with a visible side effect.
TEST_F(PicoWebTransportBaseTest, ResetQueuedMidBurstIsDeliveredOnNextPull) {
  auto* busy = createUni();
  ASSERT_NE(busy, nullptr);
  auto busyId = busy->getID();
  busy->writeStreamData(folly::IOBuf::copyBuffer("busy"), false, nullptr);

  auto* doomed = createUni();
  ASSERT_NE(doomed, nullptr);
  auto doomedId = doomed->getID();
  doomed->writeStreamData(folly::IOBuf::copyBuffer("doomed"), false, nullptr);
  doomed->resetStream(42);
  ASSERT_THAT(wt_->resets, IsEmpty());

  wt_->pull(busyId);

  EXPECT_THAT(wt_->resets, ElementsAre(Pair(doomedId, 42u)));
}

} // namespace moxygen::test
