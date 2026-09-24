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
 * responsiveness for fewer syscalls, since the EventBase loop is not serviced
 * until a drain yields. The defaults sit well above picoquic's own sockloop,
 * which sends at most PICOQUIC_PACKET_LOOP_SEND_MAX (10) packets per call.
 */
struct PicoSocketConfig {
  // A msg is one mmsghdr slot; a batch is one sendmmsg call.
  size_t maxMsgsPerBatch{32};
  size_t maxBytesPerBatch{64 * 1024};

  // The kernel caps a GSO datagram at 64KB of payload and at UDP_MAX_SEGMENTS
  // segments; these stay well inside both.
  size_t maxSegmentsPerMsg{32};
  size_t maxBytesPerMsg{45000};

  // Packets pulled per drainOutgoing call, bounding how long one drain may
  // starve the other handlers.
  size_t maxPacketsPerDrain{64};

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
  bool disableMigration{false};        // disable active connection migration

  // Context-level defaults
  uint8_t defaultStreamPriority{2};   // default stream priority
  uint8_t defaultDatagramPriority{1}; // default datagram priority
  std::string ccAlgo{"bbr"};          // congestion control algorithm name
  uint32_t mtuMax{1500};              // EMSGSIZE if above real link MTU

  PicoSocketConfig socket{}; // socket handler tuning, not passed to picoquic
};

} // namespace moxygen
