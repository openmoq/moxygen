/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "moxygen/openmoq/transport/pico/PicoQuicSocketHandler.h"
#include <folly/String.h>
#include <folly/logging/xlog.h>
#include <folly/net/NetOps.h>
#include <folly/net/NetworkSocket.h>
#include <picoquic.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip_icmp.h>
#include <netinet/udp.h>
#include <sys/socket.h>
#ifdef __linux__
#include <linux/errqueue.h>
#endif

// IP_PKTINFO (Linux) provides dst addr + ifindex in one cmsg.
// On macOS/BSD, use IP_RECVDSTADDR for dst addr instead.
#ifndef IP_PKTINFO
#ifdef IP_RECVDSTADDR
#define MOXYGEN_USE_IP_RECVDSTADDR 1
#endif
#endif

// IPV6_RECVPKTINFO (POSIX RFC 3542) enables IPv6 pktinfo cmsgs.
// Fall back to IPV6_PKTINFO on older platforms that lack it.
#ifndef IPV6_RECVPKTINFO
#define IPV6_RECVPKTINFO IPV6_PKTINFO
#endif

namespace moxygen {

namespace {

constexpr int kRecvBatchSize = 16;
constexpr size_t kMaxPacketSize = 1500;
// cmsg buffer per message: space for IP_PKTINFO/IPV6_PKTINFO + IP_TOS.
constexpr size_t kCmsgBufSize =
    CMSG_SPACE(sizeof(struct in6_pktinfo)) + CMSG_SPACE(sizeof(uint8_t));
// Max wake delay passed to picoquic (200 ms in microseconds).
// This is does not impact loop latency, whenever the wake delay shrinks, we
// reschedule
constexpr int64_t kMaxWakeDelayUs = 200'000;

// Room one picoquic_prepare_next_packet_ex call needs: picoquic coalesces a
// train of same-path packets into the buffer it is handed.
constexpr size_t kPrepareBufSize = kMaxPacketSize * 10;
// cmsg space per message: pktinfo plus UDP_SEGMENT.
constexpr size_t kSendCmsgBufSize =
    CMSG_SPACE(sizeof(struct in6_pktinfo)) + CMSG_SPACE(sizeof(uint16_t));

// Convert an AF_INET sockaddr_storage to its IPv4-mapped AF_INET6 equivalent.
// Needed when sending over a dual-stack AF_INET6 socket to an IPv4 peer:
// picoquic normalises IPv4-mapped addresses to AF_INET internally, but
// sendmsg on an AF_INET6 fd requires AF_INET6 (IPv4-mapped) addresses.
sockaddr_storage toMappedV6(const sockaddr_storage& in) {
  sockaddr_storage out{};
  const auto* src = reinterpret_cast<const sockaddr_in*>(&in);
  auto* dst = reinterpret_cast<sockaddr_in6*>(&out);
  dst->sin6_family = AF_INET6;
  dst->sin6_port = src->sin_port;
  // ::ffff:x.x.x.x
  dst->sin6_addr.s6_addr[10] = 0xff;
  dst->sin6_addr.s6_addr[11] = 0xff;
  memcpy(&dst->sin6_addr.s6_addr[12], &src->sin_addr, 4);
  return out;
}

// Whole-struct memcmp is unsafe here: picoquic leaves the sockaddr_storage
// padding uninitialised.
bool sameAddr(const sockaddr_storage& a, const sockaddr_storage& b) {
  if (a.ss_family != b.ss_family) {
    return false;
  }
  if (a.ss_family == AF_INET6) {
    const auto* x = reinterpret_cast<const sockaddr_in6*>(&a);
    const auto* y = reinterpret_cast<const sockaddr_in6*>(&b);
    return x->sin6_port == y->sin6_port &&
        x->sin6_scope_id == y->sin6_scope_id &&
        memcmp(&x->sin6_addr, &y->sin6_addr, sizeof(struct in6_addr)) == 0;
  }
  if (a.ss_family == AF_INET) {
    const auto* x = reinterpret_cast<const sockaddr_in*>(&a);
    const auto* y = reinterpret_cast<const sockaddr_in*>(&b);
    return x->sin_port == y->sin_port &&
        x->sin_addr.s_addr == y->sin_addr.s_addr;
  }
  // Both unset: neither message gets a pktinfo cmsg, so they still match.
  return true;
}

} // namespace

PicoQuicSocketHandler::PicoQuicSocketHandler(
    folly::EventBase* evb,
    picoquic_quic_t* quic,
    PicoSocketConfig socketConfig)
    : socket_(evb),
      quic_(quic),
      evb_(evb),
      config_(socketConfig),
      wakeTimeoutManager_(evb),
      wakeTimeout_(&wakeTimeoutManager_, this),
      writeReadyHandler_(evb, this) {
  // A batch with no slot to fill or no packet to pull never drains.
  XCHECK_GE(config_.maxMsgsPerBatch, 1u);
  XCHECK_GE(config_.maxPacketsPerDrain, 1u);
  XCHECK_GE(config_.maxPacketsPerRead, 1u);
  XCHECK_GE(config_.maxSegmentsPerMsg, 1u);
  // One prepare call's worth of headroom past the byte budget, so the last
  // packet of a batch always has somewhere to land.
  sendArena_.resize(config_.maxBatchBytes + kPrepareBufSize);
  cmsgArena_.resize(config_.maxMsgsPerBatch * kSendCmsgBufSize);
  slots_.resize(config_.maxMsgsPerBatch);
  msgs_.resize(config_.maxMsgsPerBatch);
  iovs_.resize(config_.maxMsgsPerBatch);
}

PicoQuicSocketHandler::~PicoQuicSocketHandler() {
  stop();
}

void PicoQuicSocketHandler::start(
    const folly::SocketAddress& addr,
    bool reusePort) {
  XLOG(DBG1) << "PicoQuicSocketHandler::start called, addr=" << addr.describe();

  // Must precede bind() below — AsyncUDPSocket applies it at fd creation time.
  socket_.setReusePort(reusePort);

  // bindV6Only defaults to true, which leaves a "::" listener unreachable from
  // IPv4 peers. Only init() applies it, so it must ride on the bind call.
  folly::AsyncUDPSocket::BindOptions bindOptions;
  bindOptions.bindV6Only = false;
  socket_.bind(addr, bindOptions);
  fd_ = socket_.getNetworkSocket().toFd();
  socketFamily_ = addr.getFamily();
  localPort_ = socket_.address().getPort();
  XLOG(DBG4) << "Socket bound, fd=" << fd_ << " localPort=" << localPort_;

  // Enable IP_PKTINFO / IPV6_RECVPKTINFO so recvmsg delivers the local
  // destination address. AsyncUDPSocket does not set these by default.
  int one = 1;
  if (addr.getFamily() == AF_INET6) {
    if (::setsockopt(fd_, IPPROTO_IPV6, IPV6_RECVPKTINFO, &one, sizeof(one)) <
        0) {
      XLOG(WARN) << "setsockopt IPV6_RECVPKTINFO failed: "
                 << folly::errnoStr(errno);
    }
  } else {
#ifdef MOXYGEN_USE_IP_RECVDSTADDR
    if (::setsockopt(fd_, IPPROTO_IP, IP_RECVDSTADDR, &one, sizeof(one)) < 0) {
      XLOG(WARN) << "setsockopt IP_RECVDSTADDR failed: "
                 << folly::errnoStr(errno);
    }
#else
    if (::setsockopt(fd_, IPPROTO_IP, IP_PKTINFO, &one, sizeof(one)) < 0) {
      XLOG(WARN) << "setsockopt IP_PKTINFO failed: " << folly::errnoStr(errno);
    }
#endif
  }

  // picoquic's own packet loop sizes this
  // (packet_loop_param::socket_buffer_size); unset, the socket falls back to
  // net.core.wmem_default (208KB by default), which one shared socket fanning
  // out to hundreds of connections fills constantly, costing an EPOLLOUT
  // round-trip per batch the socket refuses.
  int sockBuf = config_.socketBufferBytes;
  if (::setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &sockBuf, sizeof(sockBuf)) < 0) {
    XLOG(WARN) << "setsockopt SO_SNDBUF failed: " << folly::errnoStr(errno);
  }
  if (::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &sockBuf, sizeof(sockBuf)) < 0) {
    XLOG(WARN) << "setsockopt SO_RCVBUF failed: " << folly::errnoStr(errno);
  }

  // ECN receive (sets IP_RECVTOS + IPV6_RECVTCLASS).
  socket_.setRecvTos(true);

  // GRO if available.
  if (socket_.getGRO() >= 0) {
    socket_.setGRO(true);
  }

  // GSO availability.
  gsoSupported_ = (socket_.getGSO() >= 0);

  socket_.setErrMessageCallback(this);
  socket_.resumeRead(this);
  writeReadyHandler_.changeHandlerFD(folly::NetworkSocket::fromFd(fd_));
  rescheduleTimer();

  XLOG(INFO) << "PicoQuicSocketHandler started on "
             << socket_.address().describe() << " gso=" << gsoSupported_;
}

void PicoQuicSocketHandler::stop() {
  if (stopped_) {
    return;
  }
  stopped_ = true;
  wakeTimeout_.cancelTimeout();
  unregisterWrite();
  // Let picoquic's close packets out, then abandon anything still queued.
  resetBatch();
  drainOutgoing();
  resetBatch();
  if (sendCalls_ > 0) {
    XLOG(INFO) << "socket sends: " << sendCalls_ << " sendmmsg calls, "
               << sendMessages_ << " messages, " << sendDatagrams_
               << " datagrams ("
               << (static_cast<double>(sendDatagrams_) / sendCalls_)
               << " datagrams/call), " << sendEagain_ << " write-blocked, "
               << sendDropped_ << " dropped";
  }
  pauseRead();
}

void PicoQuicSocketHandler::closeMaybeDeferred() {
  pendingClose_ = true;
}

void PicoQuicSocketHandler::pauseRead() {
  if (socket_.isBound()) {
    socket_.pauseRead();
    socket_.setErrMessageCallback(nullptr);
  }
}

// ---------------------------------------------------------------------------
// AsyncUDPSocket::ReadCallback — notify-only
// ---------------------------------------------------------------------------

bool PicoQuicSocketHandler::shouldOnlyNotify() {
  return true;
}

void PicoQuicSocketHandler::onNotifyDataAvailable(
    folly::AsyncUDPSocket& sock) noexcept {
  struct mmsghdr msgs[kRecvBatchSize];
  struct iovec iovecs[kRecvBatchSize];
  uint8_t bufs[kRecvBatchSize][kMaxPacketSize];
  sockaddr_storage fromAddrs[kRecvBatchSize];
  char cmsgBufs[kRecvBatchSize][kCmsgBufSize];

  for (int i = 0; i < kRecvBatchSize; i++) {
    iovecs[i].iov_base = bufs[i];
    iovecs[i].iov_len = kMaxPacketSize;
    msgs[i].msg_hdr.msg_name = &fromAddrs[i];
    msgs[i].msg_hdr.msg_namelen = sizeof(fromAddrs[i]);
    msgs[i].msg_hdr.msg_iov = &iovecs[i];
    msgs[i].msg_hdr.msg_iovlen = 1;
    msgs[i].msg_hdr.msg_control = cmsgBufs[i];
    msgs[i].msg_hdr.msg_controllen = kCmsgBufSize;
    msgs[i].msg_hdr.msg_flags = 0;
    msgs[i].msg_len = 0;
  }

  bool anyReceived = false;
  int totalReceived = 0;
  for (;;) {
    int n = sock.recvmmsg(msgs, kRecvBatchSize, MSG_DONTWAIT, nullptr);
    if (n <= 0) {
      break;
    }
    anyReceived = true;
    totalReceived += n;

    uint64_t currentTime = picoquic_current_time();
    for (int i = 0; i < n; i++) {
      parseCmsgsAndDeliver(msgs[i], bufs[i], currentTime);
      msgs[i].msg_hdr.msg_namelen = sizeof(fromAddrs[i]);
      msgs[i].msg_hdr.msg_controllen = kCmsgBufSize;
      msgs[i].msg_hdr.msg_flags = 0;
      msgs[i].msg_len = 0;
    }

    // Level-triggered: what is left wakes us again after the send path runs.
    if (totalReceived >= static_cast<int>(config_.maxPacketsPerRead)) {
      break;
    }
  }

  if (anyReceived) {
    XLOG(DBG5) << "onNotifyDataAvailable: received " << totalReceived
               << " packets, draining";
    if (statsCallback_) {
      statsCallback_->onPacketsReceived(static_cast<uint64_t>(totalReceived));
    }
    drainOutgoing();
    if (pendingClose_) {
      stop();
    } else {
      rescheduleTimer();
    }
  }
}

void PicoQuicSocketHandler::getReadBuffer(
    void** /*buf*/,
    size_t* /*len*/) noexcept {
  XLOG(WARN)
      << "getReadBuffer called (should not happen with shouldOnlyNotify)";
}

void PicoQuicSocketHandler::onDataAvailable(
    const folly::SocketAddress& /*client*/,
    size_t /*len*/,
    bool /*truncated*/,
    OnDataAvailableParams /*params*/) noexcept {
  XLOG(WARN)
      << "onDataAvailable called (should not happen with shouldOnlyNotify)";
}

void PicoQuicSocketHandler::onReadError(
    const folly::AsyncSocketException& ex) noexcept {
  XLOG(ERR) << "UDP read error: " << ex.what();
}

void PicoQuicSocketHandler::onReadClosed() noexcept {
  XLOG(DBG1) << "UDP socket closed";
}

// ---------------------------------------------------------------------------
// Wake timer — picoquic's next-wake delay, scheduled on wakeTimeoutManager_
// ---------------------------------------------------------------------------

void PicoQuicSocketHandler::onWakeTimeout() noexcept {
  drainOutgoing();
  if (pendingClose_) {
    stop();
  } else {
    rescheduleTimer();
  }
}

// ---------------------------------------------------------------------------
// AsyncUDPSocket::ErrMessageCallback — ICMP errors
// ---------------------------------------------------------------------------

void PicoQuicSocketHandler::errMessage(const cmsghdr& cmsg) noexcept {
#ifdef __linux__
  if ((cmsg.cmsg_level == SOL_IP && cmsg.cmsg_type == IP_RECVERR) ||
      (cmsg.cmsg_level == SOL_IPV6 && cmsg.cmsg_type == IPV6_RECVERR)) {
    const auto* ee =
        reinterpret_cast<const struct sock_extended_err*>(CMSG_DATA(&cmsg));
    XLOG(WARN) << "ICMP error from peer: origin=" << (int)ee->ee_origin
               << " type=" << (int)ee->ee_type << " code=" << (int)ee->ee_code
               << " errno=" << (int)ee->ee_errno;
    // TODO: call picoquic_notify_destination_unreachable(cnx, ...) to let
    // picoquic fail the path immediately.  Requires a cnx lookup by peer
    // address; picoquic has no public API for that yet.
  }
#endif
}

void PicoQuicSocketHandler::errMessageError(
    const folly::AsyncSocketException& ex) noexcept {
  XLOG(WARN) << "Error reading error queue: " << ex.what();
}

// ---------------------------------------------------------------------------
// I/O helpers
// ---------------------------------------------------------------------------

void PicoQuicSocketHandler::parseCmsgsAndDeliver(
    const struct mmsghdr& msg,
    const uint8_t* pkt,
    uint64_t currentTime) {
  XLOG(DBG5) << "parseCmsgsAndDeliver: pktLen=" << msg.msg_len;
  sockaddr_storage addrTo{};
  int ifIndex = 0;
  unsigned char ecn = 0;

  for (auto* cmsg = CMSG_FIRSTHDR(&msg.msg_hdr); cmsg != nullptr;
       cmsg = CMSG_NXTHDR(const_cast<struct msghdr*>(&msg.msg_hdr), cmsg)) {
    if (cmsg->cmsg_level == IPPROTO_IP) {
#ifdef MOXYGEN_USE_IP_RECVDSTADDR
      if (cmsg->cmsg_type == IP_RECVDSTADDR) {
        auto* dst = reinterpret_cast<sockaddr_in*>(&addrTo);
        dst->sin_family = AF_INET;
        dst->sin_port = htons(localPort_);
        dst->sin_addr = *reinterpret_cast<struct in_addr*>(CMSG_DATA(cmsg));
      } else
#else
      if (cmsg->cmsg_type == IP_PKTINFO) {
        auto* pki = reinterpret_cast<struct in_pktinfo*>(CMSG_DATA(cmsg));
        auto* dst = reinterpret_cast<sockaddr_in*>(&addrTo);
        dst->sin_family = AF_INET;
        dst->sin_port = htons(localPort_);
        dst->sin_addr = pki->ipi_addr;
        ifIndex = static_cast<int>(pki->ipi_ifindex);
      } else
#endif
          if (cmsg->cmsg_type == IP_TOS || cmsg->cmsg_type == IP_RECVTOS) {
        ecn = *reinterpret_cast<unsigned char*>(CMSG_DATA(cmsg));
      }
    } else if (cmsg->cmsg_level == IPPROTO_IPV6) {
      if (cmsg->cmsg_type == IPV6_PKTINFO) {
        auto* pki6 = reinterpret_cast<struct in6_pktinfo*>(CMSG_DATA(cmsg));
        auto* dst = reinterpret_cast<sockaddr_in6*>(&addrTo);
        dst->sin6_family = AF_INET6;
        dst->sin6_port = htons(localPort_);
        dst->sin6_addr = pki6->ipi6_addr;
        ifIndex = static_cast<int>(pki6->ipi6_ifindex);
      } else if (cmsg->cmsg_type == IPV6_TCLASS) {
        ecn = *reinterpret_cast<unsigned char*>(CMSG_DATA(cmsg));
      }
    }
  }

  // Log address info for debugging dual-stack issues
  auto* fromAddr =
      reinterpret_cast<const sockaddr_storage*>(msg.msg_hdr.msg_name);
  XLOG(DBG6) << "parseCmsgsAndDeliver: from.family=" << fromAddr->ss_family
             << " to.family=" << addrTo.ss_family << " pktLen=" << msg.msg_len;

  picoquic_cnx_t* lastCnx = nullptr;
  int ret = picoquic_incoming_packet_ex(
      quic_,
      const_cast<uint8_t*>(pkt),
      static_cast<size_t>(msg.msg_len),
      reinterpret_cast<sockaddr*>(const_cast<sockaddr_storage*>(
          reinterpret_cast<const sockaddr_storage*>(msg.msg_hdr.msg_name))),
      reinterpret_cast<sockaddr*>(&addrTo),
      ifIndex,
      ecn,
      &lastCnx,
      currentTime);

  if (ret != 0) {
    XLOG(DBG4) << "picoquic_incoming_packet_ex returned " << ret;
  }
}

void PicoQuicSocketHandler::drainOutgoing() {
  const uint64_t datagramsBefore = sendDatagrams_;

  // Nothing may be pulled from picoquic while the socket still owes us the
  // previous batch; each packet pulled is one picoquic already counts in
  // flight, and it can never learn that one never left the host.
  if (sendBatch()) {
    uint64_t currentTime = picoquic_current_time();
    size_t packetsPrepared = 0;
    while (packetsPrepared < config_.maxPacketsPerDrain) {
      size_t prepared = fillBatch(currentTime);
      if (prepared == 0) {
        XLOG(DBG5) << "drainOutgoing: done, prepared=" << packetsPrepared;
        break;
      }
      packetsPrepared += prepared;
      if (!sendBatch()) {
        break;
      }
    }
  }

  // Report what the socket took, not what picoquic handed us: a blocked batch
  // is counted when the EPOLLOUT retry finally pushes it.
  if (statsCallback_ && sendDatagrams_ > datagramsBefore) {
    statsCallback_->onPacketsSent(sendDatagrams_ - datagramsBefore);
  }
}

size_t PicoQuicSocketHandler::fillBatch(uint64_t currentTime) {
  XCHECK_EQ(batchCount_, 0u) << "fillBatch over an unsent batch";
  writeOffset_ = 0;
  size_t prepared = 0;

  while (batchCount_ < config_.maxMsgsPerBatch &&
         writeOffset_ + kPrepareBufSize <= sendArena_.size()) {
    size_t sendLength = 0;
    size_t sendMsgSize = 0;
    sockaddr_storage addrTo{};
    sockaddr_storage addrFrom{};
    int ifIndex = 0;
    picoquic_connection_id_t logCid{};
    picoquic_cnx_t* lastCnx = nullptr;

    // Without a send_msg_size pointer picoquic emits one packet per call
    // instead of a coalesced train, which is what we want with no GSO: the
    // train would otherwise go out as one oversized, IP-fragmented datagram.
    int ret = picoquic_prepare_next_packet_ex(
        quic_,
        currentTime,
        sendArena_.data() + writeOffset_,
        kPrepareBufSize,
        &sendLength,
        &addrTo,
        &addrFrom,
        &ifIndex,
        &logCid,
        &lastCnx,
        gsoSupported_ ? &sendMsgSize : nullptr);

    if (ret != 0 || sendLength == 0) {
      break;
    }
    ++prepared;
    appendToBatch(sendLength, sendMsgSize, addrTo, addrFrom, ifIndex);
  }

  if (batchCount_ > 0) {
    finalizeSlot(batchCount_ - 1);
  }
  return prepared;
}

void PicoQuicSocketHandler::appendToBatch(
    size_t length,
    size_t sendMsgSize,
    const sockaddr_storage& addrTo,
    const sockaddr_storage& addrFrom,
    int ifIndex) {
  // picoquic normalises IPv4-mapped peer addresses to AF_INET internally.
  // On a dual-stack AF_INET6 socket, sendmsg requires AF_INET6 (IPv4-mapped)
  // addresses — IP-level cmsg options (IP_PKTINFO) are rejected on an
  // AF_INET6 fd.  Promote both addresses for this packet when needed.
  sockaddr_storage effectiveTo = addrTo;
  sockaddr_storage effectiveFrom = addrFrom;
  if (socketFamily_ == AF_INET6 && addrTo.ss_family == AF_INET) {
    effectiveTo = toMappedV6(addrTo);
    if (addrFrom.ss_family == AF_INET) {
      effectiveFrom = toMappedV6(addrFrom);
    }
  }

  // picoquic reports sendMsgSize as the path MTU even for a lone packet, so a
  // train is only one when the buffer is longer than a segment.
  const bool isTrain = sendMsgSize > 0 && sendMsgSize < length;
  const size_t segSize = isTrain ? sendMsgSize : length;
  const size_t segCount = isTrain ? (length + segSize - 1) / segSize : 1;
  const bool shortTail = (length % segSize) != 0;

  if (gsoSupported_ && batchCount_ > 0) {
    SendSlot& run = slots_[batchCount_ - 1];
    const bool sameSegmentation =
        isTrain ? segSize == run.segSize : length <= run.segSize;
    if (!run.sealed && sameSegmentation && run.ifIndex == ifIndex &&
        sameAddr(run.addrTo, effectiveTo) &&
        sameAddr(run.addrFrom, effectiveFrom) &&
        run.segCount + segCount <= config_.maxSegmentsPerMsg &&
        run.length + length <= config_.maxGsoRunBytes) {
      run.length += length;
      run.segCount += segCount;
      run.sealed = isTrain ? shortTail : length != run.segSize;
      writeOffset_ += length;
      return;
    }
  }

  if (batchCount_ > 0) {
    finalizeSlot(batchCount_ - 1);
  }
  SendSlot& slot = slots_[batchCount_];
  slot.offset = writeOffset_;
  slot.length = length;
  slot.segSize = segSize;
  slot.segCount = segCount;
  slot.addrTo = effectiveTo;
  slot.addrFrom = effectiveFrom;
  slot.ifIndex = ifIndex;
  slot.sealed = shortTail;
  ++batchCount_;
  writeOffset_ += length;
}

void PicoQuicSocketHandler::finalizeSlot(size_t index) {
  SendSlot& slot = slots_[index];
  char* cmsgBuf = cmsgArena_.data() + index * kSendCmsgBufSize;

  iovs_[index].iov_base = sendArena_.data() + slot.offset;
  iovs_[index].iov_len = slot.length;

  struct msghdr& msg = msgs_[index].msg_hdr;
  msg = {};
  msgs_[index].msg_len = 0;
  msg.msg_name = &slot.addrTo;
  msg.msg_namelen = (slot.addrTo.ss_family == AF_INET6) ? sizeof(sockaddr_in6)
                                                        : sizeof(sockaddr_in);
  msg.msg_iov = &iovs_[index];
  msg.msg_iovlen = 1;
  msg.msg_control = cmsgBuf;
  msg.msg_controllen = kSendCmsgBufSize;

  struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
  size_t controlLen = 0;

  // Only set pktinfo if addrFrom is valid — matches picoquic sockloop behavior.
  if (slot.addrFrom.ss_family == AF_INET6) {
    cmsg->cmsg_level = IPPROTO_IPV6;
    cmsg->cmsg_type = IPV6_PKTINFO;
    cmsg->cmsg_len = CMSG_LEN(sizeof(struct in6_pktinfo));
    auto* pki6 = reinterpret_cast<struct in6_pktinfo*>(CMSG_DATA(cmsg));
    pki6->ipi6_addr =
        reinterpret_cast<const sockaddr_in6*>(&slot.addrFrom)->sin6_addr;
    pki6->ipi6_ifindex = static_cast<unsigned>(slot.ifIndex);
    controlLen += CMSG_SPACE(sizeof(struct in6_pktinfo));
  } else if (slot.addrFrom.ss_family == AF_INET) {
#ifdef MOXYGEN_USE_IP_RECVDSTADDR
    // macOS/BSD: use IP_SENDSRCADDR (struct in_addr, no ifindex).
    cmsg->cmsg_level = IPPROTO_IP;
    cmsg->cmsg_type = IP_SENDSRCADDR;
    cmsg->cmsg_len = CMSG_LEN(sizeof(struct in_addr));
    *reinterpret_cast<struct in_addr*>(CMSG_DATA(cmsg)) =
        reinterpret_cast<const sockaddr_in*>(&slot.addrFrom)->sin_addr;
    controlLen += CMSG_SPACE(sizeof(struct in_addr));
#else
    cmsg->cmsg_level = IPPROTO_IP;
    cmsg->cmsg_type = IP_PKTINFO;
    cmsg->cmsg_len = CMSG_LEN(sizeof(struct in_pktinfo));
    auto* pki = reinterpret_cast<struct in_pktinfo*>(CMSG_DATA(cmsg));
    pki->ipi_spec_dst =
        reinterpret_cast<const sockaddr_in*>(&slot.addrFrom)->sin_addr;
    pki->ipi_ifindex = static_cast<unsigned long>(slot.ifIndex);
    controlLen += CMSG_SPACE(sizeof(struct in_pktinfo));
#endif
  }

#if defined(UDP_SEGMENT)
  if (gsoSupported_ && slot.segCount > 1) {
    cmsg = reinterpret_cast<struct cmsghdr*>(cmsgBuf + controlLen);
    cmsg->cmsg_level = SOL_UDP;
    cmsg->cmsg_type = UDP_SEGMENT;
    cmsg->cmsg_len = CMSG_LEN(sizeof(uint16_t));
    *reinterpret_cast<uint16_t*>(CMSG_DATA(cmsg)) =
        static_cast<uint16_t>(slot.segSize);
    controlLen += CMSG_SPACE(sizeof(uint16_t));
  }
#endif

  msg.msg_controllen = controlLen;
  if (controlLen == 0) {
    msg.msg_control = nullptr;
  }
}

bool PicoQuicSocketHandler::sendBatch() {
  while (batchSent_ < batchCount_) {
    auto remaining = static_cast<unsigned int>(batchCount_ - batchSent_);
    sendCalls_++;
    int ret = folly::netops::sendmmsg(
        folly::NetworkSocket::fromFd(fd_),
        msgs_.data() + batchSent_,
        remaining,
        0);
    if (ret > 0) {
      size_t accepted = static_cast<size_t>(ret);
      for (size_t i = batchSent_; i < batchSent_ + accepted; i++) {
        sendDatagrams_ += slots_[i].segCount;
      }
      batchSent_ += accepted;
      sendMessages_ += accepted;
      if (batchSent_ == batchCount_) {
        break;
      }
      // A short count means the kernel refused the next message. errno is not
      // dependable after a partial sendmmsg, so treat it as backpressure and
      // let the EPOLLOUT retry resolve it: a genuinely fatal message errors
      // out on its own the moment the socket reports writable again.
      sendEagain_++;
      registerWrite();
      return false;
    }

    int err = errno;
    if (err == EAGAIN || err == EWOULDBLOCK) {
      sendEagain_++;
      registerWrite();
      return false;
    }
    // Nothing to retry for: drop this one message and carry on with the rest.
    // ENOBUFS too: it is the qdisc, not the socket buffer, so a retry spins.
    const SendSlot& slot = slots_[batchSent_];
    if (err == EIO && slot.segCount > 1) {
      // The kernel took UDP_SEGMENT but this driver will not, so every
      // segmented batch fails the same way until we stop building them.
      gsoSupported_ = false;
      XLOG(WARN) << "GSO disabled: EIO on a segmented send";
    }
    sendDropped_++;
    XLOG_EVERY_N(WARN, 100)
        << "sendmmsg failed: " << folly::errnoStr(err)
        << " addrFrom.family=" << slot.addrFrom.ss_family
        << " length=" << slot.length << " segSize=" << slot.segSize
        << " segCount=" << slot.segCount << " gsoSupported=" << gsoSupported_;
    ++batchSent_;
  }

  resetBatch();
  unregisterWrite();
  return true;
}

void PicoQuicSocketHandler::resetBatch() {
  batchCount_ = 0;
  batchSent_ = 0;
  writeOffset_ = 0;
}

void PicoQuicSocketHandler::registerWrite() {
  if (writeRegistered_ || stopped_) {
    return;
  }
  writeReadyHandler_.registerHandler(
      folly::EventHandler::WRITE | folly::EventHandler::PERSIST);
  writeRegistered_ = true;
}

void PicoQuicSocketHandler::unregisterWrite() {
  if (!writeRegistered_) {
    return;
  }
  writeReadyHandler_.unregisterHandler();
  writeRegistered_ = false;
}

void PicoQuicSocketHandler::onSocketWritable() {
  if (stopped_) {
    return;
  }
  // drainOutgoing retries the pending batch first; rescheduleTimer is a no-op
  // while that batch is still unsent.
  drainOutgoing();
  rescheduleTimer();
}

void PicoQuicSocketHandler::updateWakeTimeout() {
  // Called via WakeTimeGuard when picoquic's next wake time decreases (e.g.
  // after marking a stream or datagram active). Cancel the current timer and
  // reschedule: rescheduleTimer will call evb_->add() for delay<=0, which is
  // effectively immediate since the EVB passes 0 to epoll when tasks are
  // pending.
  wakeTimeout_.cancelTimeout();
  rescheduleTimer();
}

void PicoQuicSocketHandler::rescheduleTimer() {
  // While the socket still owes us a batch, picoquic reports a zero wake delay
  // for every connection that wants to send, so rearming here would spin the
  // loop until the socket unblocks. EPOLLOUT restarts the drain instead.
  if (batchSent_ < batchCount_) {
    return;
  }
  uint64_t now = picoquic_current_time();
  int64_t rawDelayUs = picoquic_get_next_wake_delay(quic_, now, INT64_MAX);
  int64_t delayUs = std::min(rawDelayUs, kMaxWakeDelayUs);
  if (delayUs <= 0) {
    evb_->add([this] {
      drainOutgoing();
      rescheduleTimer();
    });
  } else {
    // wakeTimeoutManager_ is a timerfd-backed TimeoutManager: unlike the
    // owning EventBase (whose scheduleTimeoutHighRes silently ceils to whole
    // milliseconds), it honors microsecond delays, which picoquic's pacing
    // relies on to avoid releasing packets in coarse, bursty batches.
    wakeTimeout_.scheduleTimeoutHighRes(std::chrono::microseconds(delayUs));
  }
}

} // namespace moxygen
