/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>

#include <moxygen/util/RecentGroupWindow.h>

namespace moxygen {

// The data of one subscription, PUBLISH or FETCH, counted by MoQSession. Only
// code on the session's executor may read or update it.
struct MoQTrackStats {
  // Data objects and their payload bytes: those the app handed to the session
  // when sending, and those the session delivered to the app when receiving.
  // Status objects do not count.
  uint64_t objects{0};
  uint64_t bytes{0};
  uint64_t groups{0};
  uint64_t subgroups{0};
  // Data datagrams. A status datagram counts in statusObjects.
  uint64_t datagrams{0};
  // Status objects, such as end of group, that were written or received.
  uint64_t statusObjects{0};
  // Subgroup and fetch streams that either side reset, and the last code.
  uint64_t resets{0};
  std::optional<uint64_t> lastResetCode;
  // Subgroup streams open now.
  uint64_t openSubgroups{0};
  // When sending, headers included: bytes queued for the transport, those
  // written to it, and the stream bytes that the peer acknowledged. Queued
  // bytes that a reset or a write error discards count in bytesDropped, and
  // written bytes whose delivery a reset cancels count in bytesCancelled. So
  // queued - written - dropped waits for stream credit, and written - acked -
  // cancelled is in flight.
  uint64_t bytesQueued{0};
  // The payload within bytesQueued. The rest is headers.
  uint64_t payloadBytesQueued{0};
  uint64_t bytesWritten{0};
  uint64_t bytesAcked{0};
  uint64_t bytesDropped{0};
  uint64_t bytesCancelled{0};
  // When sending, the bytes of datagrams written to the transport, headers
  // included. The stream byte counts above leave them out.
  uint64_t datagramBytes{0};
  // When sending: data objects written to the transport, and those that the
  // peer acknowledged. objectsAcked and the ack latencies count only while the
  // session tracks deliveries, which needs a delivery callback, a delivery
  // timeout or a publisher stats callback. Datagrams are not acknowledged.
  uint64_t objectsWritten{0};
  uint64_t objectsAcked{0};
  // From queueing each acked object to its ack, in microseconds.
  uint64_t ackLatencyUsSum{0};
  uint64_t ackLatencyUsMax{0};
  std::optional<std::chrono::steady_clock::time_point> firstObject;
  RecentGroupWindow recentGroups;

  // The session calls this once per stream for each group that the stream
  // carries, and once per datagram, so objects do not age groups out.
  void onGroup(uint64_t group);
  void onObject(uint64_t payloadBytes);
  // A data object on a stream. streamGroup is the group that the stream last
  // counted.
  void onStreamObject(
      std::optional<uint64_t>& streamGroup,
      uint64_t group,
      uint64_t payloadBytes);
  void onDatagram(uint64_t group, uint64_t payloadBytes);
  void onSubgroupOpened();
  void onSubgroupClosed();
  void onQueued(uint64_t wireBytes, uint64_t payloadBytes);
  void onWritten(uint64_t wireBytes, uint64_t dataObjects);
  void onAcked(uint64_t latencyUs);
  void onReset(uint64_t code);
};

// Implemented by the consumers and handles that MoQSession provides. The
// consumer and handle interfaces are also implemented by the app, so the
// counts are not part of them.
class MoQTrackStatsSource {
 public:
  virtual ~MoQTrackStatsSource() = default;
  virtual std::shared_ptr<const MoQTrackStats> getTrackStats() const = 0;
};

// The session's counts for obj, or null when the session did not provide obj.
template <typename T>
std::shared_ptr<const MoQTrackStats> trackStatsOf(const T& obj) {
  auto* source = dynamic_cast<const MoQTrackStatsSource*>(&obj);
  return source ? source->getTrackStats() : nullptr;
}

} // namespace moxygen
