/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/io/async/AsyncTimeout.h>
#include <folly/io/async/AsyncUDPSocket.h>
#include <folly/io/async/EventHandler.h>
#include <folly/io/async/STTimerFDTimeoutManager.h>
#include <moxygen/openmoq/transport/pico/PicoQuicStatsCallback.h>
#include <moxygen/openmoq/transport/pico/PicoTransportConfig.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <vector>

// Forward declaration — avoids picoquic.h in this header
typedef struct st_picoquic_quic_t picoquic_quic_t;

namespace moxygen {

/**
 * PicoQuicSocketHandler
 *
 * Drives picoquic I/O from a folly::EventBase. Shared by both
 * MoQPicoQuicEventBaseServer and MoQPicoQuicEventBaseClient.
 *
 * Uses AsyncUDPSocket in notify-only mode so that recvmmsg is called
 * directly with a hand-crafted mmsghdr array that captures IP_PKTINFO
 * (local destination address) and TOS (ECN) — information that
 * AsyncUDPSocket's onDataAvailable callback does not expose.
 *
 * Outgoing packets are batched into an mmsghdr array and flushed with a
 * single sendmmsg, carrying IP_PKTINFO for per-packet source address control
 * and UDP_SEGMENT for GSO coalescing of consecutive same-destination packets.
 * folly's writem/writemGSO cannot express per-message pktinfo cmsgs (its
 * SocketCmsgMap only carries int-valued options), so the mmsghdr array is
 * hand-rolled the same way the receive path hand-rolls recvmmsg.
 *
 * The wake timer runs on a timerfd-backed manager because EventBase's
 * scheduleTimeoutHighRes() ceils to whole ms, too coarse for picoquic pacing.
 */
class PicoQuicSocketHandler
    : public folly::AsyncUDPSocket::ReadCallback,
      public folly::AsyncUDPSocket::ErrMessageCallback {
 public:
  PicoQuicSocketHandler(folly::EventBase* evb,
                        picoquic_quic_t* quic,
                        PicoSocketConfig socketConfig = {});
  ~PicoQuicSocketHandler() override;

  /**
   * Bind the socket to addr, set socket options, and begin receiving.
   * Must be called from the EventBase thread. reusePort sets SO_REUSEPORT so
   * several sockets can share addr.
   */
  void start(const folly::SocketAddress& addr, bool reusePort = false);

  /**
   * Cancel the wake timer, pause reads, and unbind from the EventBase.
   * Must be called from the EventBase thread.
   */
  void stop();

  /**
   * Signal that the connection has closed; stop I/O after the current drain
   * cycle. Safe to call from within a picoquic callback. Only called by the
   * EVB client — the server never calls this (handler is shared).
   */
  void closeMaybeDeferred();

  /**
   * Called when picoquic's wake time may have decreased (e.g. after
   * mark_active_stream). Cancels the pending timer, drains any ready
   * packets, and reschedules the timer at the new wake delay.
   * Must be called from the EventBase thread.
   */
  void updateWakeTimeout();

  /**
   * Returns the local address the socket is bound to (after start()).
   */
  folly::SocketAddress boundAddress() const {
    return socket_.address();
  }

  /**
   * Attach a stats callback. Must be called before start(). Non-owning.
   */
  void setStatsCallback(PicoQuicStatsCallback* cb) {
    statsCallback_ = cb;
  }

 private:
  void pauseRead();

  bool stopped_{false};
  bool pendingClose_{false};

  // AsyncUDPSocket::ReadCallback (notify-only)
  bool shouldOnlyNotify() override;
  void onNotifyDataAvailable(folly::AsyncUDPSocket& sock) noexcept override;
  void getReadBuffer(void** buf, size_t* len) noexcept override;
  void onDataAvailable(const folly::SocketAddress& client,
                       size_t len,
                       bool truncated,
                       OnDataAvailableParams params) noexcept override;
  void onReadError(const folly::AsyncSocketException& ex) noexcept override;
  void onReadClosed() noexcept override;

  // Wake timer
  void onWakeTimeout() noexcept;

  // AsyncUDPSocket::ErrMessageCallback
  void errMessage(const cmsghdr& cmsg) noexcept override;
  void errMessageError(
      const folly::AsyncSocketException& ex) noexcept override;

  // I/O helpers
  void parseCmsgsAndDeliver(const struct mmsghdr& msg,
                            const uint8_t* pkt,
                            uint64_t currentTime);
  void drainOutgoing();
  void rescheduleTimer();

  // How a prepared buffer splits into datagrams: every one is `size` bytes
  // except the last, which is `tail`. The kernel only lets the final datagram
  // be short, so tail < size means nothing may follow it in a slot.
  struct Segmentation {
    size_t size{0};
    size_t count{0};
    size_t tail{0};

    static Segmentation of(size_t length, size_t reportedSegSize);
  };

  // One mmsghdr slot: a contiguous range of arena bytes whose packets share a
  // destination, source address and interface. The kernel splits a slot of
  // several segments into that many datagrams.
  struct SendSlot {
    size_t offset{0};
    size_t length{0};
    size_t segSize{0};
    size_t segCount{0};
    sockaddr_storage addrTo{};
    sockaddr_storage addrFrom{};
    int ifIndex{0};
    // Set once a short segment lands here: nothing more may follow it.
    bool sealed{false};

    // Whether seg can be appended without breaking the segment layout.
    // reportedSegSize is picoquic's segment size for the incoming packet.
    bool segmentsFit(const Segmentation& seg, size_t reportedSegSize) const;
  };

  // The in-flight sendmmsg batch, preallocated so the send path never
  // allocates. slots, msgs and iovs are indexed together; count and sent are
  // cursors into them. Packets land back-to-back in arena, so a slot's
  // datagrams are just a byte range.
  struct SendBatch {
    explicit SendBatch(PicoSocketConfig config);

    std::vector<uint8_t> arena;
    std::vector<char> cmsgArena;
    std::vector<SendSlot> slots;
    std::vector<struct mmsghdr> msgs;
    std::vector<struct iovec> iovs;
    size_t count{0};        // slots built
    size_t sent{0};         // slots the kernel has taken
    size_t writeOffset{0};  // bytes of arena used
  };

  // Lifetime send counters, logged once at stop().
  struct SendStats {
    uint64_t eagain{0};
    uint64_t dropped{0};   // dropped on a non-retryable send error
    uint64_t calls{0};     // sendmmsg invocations
    uint64_t messages{0};  // mmsghdr slots the kernel accepted
    uint64_t datagrams{0}; // UDP datagrams those slots expand to via GSO
  };

  // Pulls packets from picoquic into the batch; returns how many datagrams it
  // pulled, which is more than the number of prepare calls when picoquic
  // returns coalesced trains.
  size_t fillBatch(uint64_t currentTime);
  // Returns the datagrams the appended buffer will expand to.
  size_t appendToBatch(
      size_t length,
      size_t sendMsgSize,
      const sockaddr_storage& addrTo,
      const sockaddr_storage& addrFrom,
      int ifIndex);
  void finalizeSlot(size_t index);
  // Sends the unsent tail of the batch. Returns false when the socket refused
  // it, leaving the remaining slots intact for the EPOLLOUT retry.
  bool sendBatch();
  void resetBatch();
  void registerWrite();
  void unregisterWrite();
  void onSocketWritable();

  class WriteReadyHandler : public folly::EventHandler {
   public:
    WriteReadyHandler(folly::EventBase* evb, PicoQuicSocketHandler* handler)
        : folly::EventHandler(evb), handler_(handler) {}
    void handlerReady(uint16_t events) noexcept override {
      if (events & folly::EventHandler::WRITE) {
        handler_->onSocketWritable();
      }
    }

   private:
    PicoQuicSocketHandler* handler_;
  };

  // Nested rather than inherited: an AsyncTimeout base would have to bind to
  // wakeTimeoutManager_ before that member finishes constructing.
  class WakeTimeout : public folly::AsyncTimeout {
   public:
    WakeTimeout(folly::TimeoutManager* mgr, PicoQuicSocketHandler* handler)
        : folly::AsyncTimeout(mgr), handler_(handler) {}
    void timeoutExpired() noexcept override {
      handler_->onWakeTimeout();
    }

   private:
    PicoQuicSocketHandler* handler_;
  };

  folly::AsyncUDPSocket socket_;
  picoquic_quic_t* quic_; // non-owning
  folly::EventBase* evb_; // non-owning
  PicoSocketConfig config_;
  PicoQuicStatsCallback* statsCallback_{nullptr}; // non-owning, optional
  int fd_{-1};
  int socketFamily_{AF_UNSPEC}; // AF_INET or AF_INET6, set in start()
  bool gsoSupported_{false};
  SendBatch batch_;
  SendStats sendStats_;
  bool writeRegistered_{false};
  uint16_t localPort_{0}; // actual bound port, for addrTo in parseCmsgsAndDeliver
  // Order matters: the manager must construct before wakeTimeout_ binds to it.
  folly::STTimerFDTimeoutManager wakeTimeoutManager_;
  WakeTimeout wakeTimeout_;
  WriteReadyHandler writeReadyHandler_;
};

} // namespace moxygen
