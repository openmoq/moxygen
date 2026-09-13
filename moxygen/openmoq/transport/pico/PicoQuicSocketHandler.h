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
 * The picoquic wake timer is driven by picoquic_get_next_wake_delay and
 * scheduled on a dedicated timerfd-backed manager (STTimerFDTimeoutManager)
 * rather than the owning EventBase directly: EventBase's own
 * scheduleTimeoutHighRes() silently ceils to whole milliseconds (see
 * TimeoutManager::scheduleTimeoutHighRes), which is too coarse for
 * picoquic's microsecond-scale pacing and causes send bursts.
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
   * Must be called from the EventBase thread. reusePort sets SO_REUSEPORT
   * so multiple sockets can share addr for sharded deployments.
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

  // Wake timer, fired by wakeTimeout_ via the timerfd-backed manager.
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

  // One mmsghdr slot: a contiguous run of bytes in sendArena_ whose packets
  // share a destination, source address and interface. A run of more than one
  // segment leaves the host as a single GSO datagram.
  struct SendSlot {
    size_t offset{0};
    size_t length{0};
    size_t segSize{0};
    size_t segCount{0};
    sockaddr_storage addrTo{};
    sockaddr_storage addrFrom{};
    int ifIndex{0};
    // The kernel only tolerates a short segment as the last one, so once a
    // segment shorter than segSize lands in the run nothing more may follow.
    bool sealed{false};
  };

  // Pulls packets from picoquic into the batch; returns how many it pulled.
  size_t fillBatch(uint64_t currentTime);
  void appendToBatch(
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

  // Fires onWakeTimeout() on the handler; declared as a nested class so the
  // handler itself need not inherit AsyncTimeout (which would force it to
  // bind to wakeTimeoutManager_ before that member finishes constructing).
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
  uint64_t sendEagain_{0};
  uint64_t sendDropped_{0};   // dropped on a non-retryable send error
  uint64_t sendCalls_{0};     // sendmmsg invocations
  uint64_t sendMessages_{0};  // mmsghdr slots the kernel accepted
  uint64_t sendDatagrams_{0}; // UDP datagrams those slots expand to via GSO

  // Batch state, all preallocated in the constructor so the send path never
  // allocates. Packets land back-to-back in sendArena_, so a GSO run is just a
  // byte range and a partial send only has to advance batchSent_.
  std::vector<uint8_t> sendArena_;
  std::vector<char> cmsgArena_;
  std::vector<SendSlot> slots_;
  std::vector<struct mmsghdr> msgs_;
  std::vector<struct iovec> iovs_;
  size_t batchCount_{0};
  size_t batchSent_{0};
  size_t writeOffset_{0};

  bool writeRegistered_{false};
  uint16_t localPort_{0}; // actual bound port, for addrTo in parseCmsgsAndDeliver
  // Declaration order matters: wakeTimeoutManager_ must construct (and
  // register its timerfd) before wakeTimeout_ attaches to it.
  folly::STTimerFDTimeoutManager wakeTimeoutManager_;
  WakeTimeout wakeTimeout_;
  WriteReadyHandler writeReadyHandler_;
};

} // namespace moxygen
