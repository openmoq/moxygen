/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "moxygen/openmoq/transport/pico/PicoQuicWebTransport.h"

#include <folly/io/IOBuf.h>
#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>
#include <picoquic.h>
#include <picoquic_internal.h>
#include <picoquic_utils.h>

using namespace testing;

namespace moxygen::test {

class PicoQuicWebTransportTest : public Test {
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
    // Stands in for the peer's transport parameters from a handshake.
    cnx_->remote_parameters.max_datagram_frame_size = 1200;

    wt_ = std::make_unique<PicoQuicWebTransport>(
        cnx_, folly::SocketAddress("127.0.0.1", 1234), peer);
  }

  void TearDown() override {
    // Destroying an open session fails XCHECK(cnx_) in ~PicoWebTransportBase.
    wt_->closeSession(0);
    wt_.reset();
    if (quic_) {
      picoquic_free(quic_);
    }
  }

  bool datagramReady() const {
    return cnx_->is_datagram_ready || cnx_->path[0]->is_datagram_ready;
  }

  // Asks for one DATAGRAM frame the way picoquic's packet builder does.
  std::string formatDatagramFrame() {
    std::array<uint8_t, 1500> packet{};
    int moreData = 0;
    int isPureAck = 1;
    int ret = 0;
    auto* end = picoquic_format_ready_datagram_frame(
        cnx_,
        cnx_->path[0],
        packet.data(),
        packet.data() + packet.size(),
        &moreData,
        &isPureAck,
        &ret);
    EXPECT_EQ(ret, 0);
    return std::string(packet.data(), end);
  }

  picoquic_quic_t* quic_{nullptr};
  picoquic_cnx_t* cnx_{nullptr};
  std::unique_ptr<PicoQuicWebTransport> wt_;
};

// picoquic polls for datagrams only while the ready flag is set. The flag must
// stay set until the queue is empty.
TEST_F(PicoQuicWebTransportTest, QueuedDatagramsDrainWithoutFurtherSends) {
  for (const char* payload : {"a", "b", "c"}) {
    ASSERT_TRUE(wt_->sendDatagram(folly::IOBuf::copyBuffer(payload)));
  }

  std::vector<std::string> frames;
  while (datagramReady() && frames.size() < 10) {
    frames.push_back(formatDatagramFrame());
  }

  // DATAGRAM with length (type 0x31), one-byte length, payload.
  auto frame = [](char payload) {
    return std::string{'\x31', '\x01', payload};
  };
  EXPECT_THAT(frames, ElementsAre(frame('a'), frame('b'), frame('c')));
  EXPECT_FALSE(datagramReady());
}

} // namespace moxygen::test
