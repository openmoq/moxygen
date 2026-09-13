/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace moxygen {

/**
 * WebTransport configuration for picoquic server.
 */
struct PicoWebTransportConfig {
  bool enableWebTransport{false}; // Enable HTTP/3 WebTransport support
  bool enableQuicTransport{
      true}; // Enable QUIC for non-browser clients (default)
  // WebTransport CONNECT endpoint paths. h3zero matches each path with
  // prefix-up-to-'?' semantics: "/moq" matches "/moq" or "/moq?..." but NOT
  // "/moq/relay". Register each path that clients will CONNECT to explicitly.
  std::vector<std::string> wtEndpoints{{"/moq"}};
  uint32_t wtMaxSessions{100}; // Max concurrent WebTransport sessions
};

/**
 * UDP send-path tuning for PicoQuicSocketHandler.
 *
 * These bound one sendmmsg batch and one drain pass. Raising them trades
 * responsiveness for fewer syscalls: a batch is only flushed once it fills or
 * picoquic runs dry, and the EventBase loop is not serviced until the drain
 * yields. The defaults are far above picoquic's own sockloop, which sends at
 * most PICOQUIC_PACKET_LOOP_SEND_MAX (10) packets per call; lower them to sit
 * closer to it.
 */
struct PicoSocketConfig {
  // mmsghdr slots per sendmmsg call, and the packet bytes one batch may hold.
  // The arena is allocated with a further packet train's worth of headroom, so
  // a batch always has room for one more prepare call.
  size_t maxMsgsPerBatch{32};
  size_t maxBatchBytes{64 * 1024};

  // Ceiling on one GSO run. The kernel caps a GSO datagram at 64KB of payload
  // and at UDP_MAX_SEGMENTS segments; these stay well inside both.
  size_t maxSegmentsPerMsg{32};
  size_t maxGsoRunBytes{45000};

  // Packets pulled from picoquic per drainOutgoing call, which bounds how long
  // one drain may starve the other handlers: on hitting it the drain returns
  // and reschedules through the loop rather than pulling further. Not a
  // per-loop-iteration ceiling — a readable socket, an expired wake timer and
  // EPOLLOUT each drain separately, so one iteration may run several.
  // A drain checks this between batches, so one batch may overshoot it.
  size_t maxPacketsPerDrain{64};

  // Packets taken from the socket per readable notification, the receive-side
  // mirror of maxPacketsPerDrain. Checked between recvmmsg calls, so one call
  // may overshoot it.
  size_t maxPacketsPerRead{64};

  // SO_SNDBUF/SO_RCVBUF for the shared socket, matching MoQServer's default.
  // The kernel clamps this to wmem_max/rmem_max, which is often far lower.
  int socketBufferBytes{1024 * 1024};
};

/**
 * QUIC transport parameter configuration for picoquic.
 *
 * Used by both server (MoQPicoServerBase) and client contexts to configure
 * flow control windows, stream limits, timeouts, and other transport
 * parameters applied to the picoquic_quic_t context.
 */
struct PicoTransportConfig {
  // Flow control (QUIC transport parameters)
  uint64_t maxData{67108864};       // connection FC window (bytes)
  uint64_t maxStreamData{16777216}; // per-stream FC window (bidi + uni)
  uint64_t maxUniStreams{8192};     // max concurrent unidirectional streams
  uint64_t maxBidiStreams{16};      // max concurrent bidirectional streams

  // Transport parameters
  uint32_t maxDatagramFrameSize{1280}; // max DATAGRAM frame size
  uint64_t idleTimeoutMs{30000};       // idle timeout (ms); handshake = /2 us
  uint32_t maxAckDelayUs{100000};      // max ACK delay (microseconds)
  uint32_t minAckDelayUs{1000};        // min ACK delay (microseconds)

  // Context-level defaults
  uint8_t defaultStreamPriority{2};   // default stream priority
  uint8_t defaultDatagramPriority{1}; // default datagram priority
  std::string ccAlgo{"bbr"};          // congestion control algorithm name

  // Real L2 MTU of the sending interface, passed to picoquic_set_mtu_max().
  // Unset, picoquic's PMTU discovery probes UDP payload sizes up to 1500
  // without reserving room for the IP/UDP header, so a standard 1500-MTU
  // link rejects every such packet with EMSGSIZE (invisible on loopback).
  uint32_t mtuMax{1500};

  // Required when sharding contexts across a shared SO_REUSEPORT port:
  // the kernel's 4-tuple hash can route a migrated connection's packets
  // to a shard that has never seen its connection ID.
  bool disableMigration{false};

  // UDP send-path tuning, applied to the socket handler rather than to the
  // picoquic context.
  PicoSocketConfig socket{};
};

} // namespace moxygen
