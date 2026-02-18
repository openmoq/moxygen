/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <moxygen/compat/Async.h>
#include <moxygen/compat/Payload.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <unordered_set>
#include <vector>

namespace moxygen::compat {

// =============================================================================
// MoQPriorityQueue - Priority queue for MoQ objects with group-level expiry
// =============================================================================
//
// Design for picoquic JIT (Just-In-Time) API:
// - Objects are enqueued by the publisher with priority and delivery deadline
// - When picoquic calls prepare_to_send, objects are dequeued by priority
// - Expired objects are dropped; if ANY object in a group expires, the ENTIRE
//   group is terminated (MoQT semantics)
// - Backpressure is signaled via Promise/Future when queue drains
//
// Priority ordering (lower number = higher priority):
//   1. Priority level (0-7, where 0 is most urgent)
//   2. Deadline (earlier deadline served first within same priority)
//   3. Object ID (tie-breaker for deterministic ordering)
//
// =============================================================================

// Queued object with metadata for priority scheduling
struct QueuedObject {
  uint64_t groupId{0};
  uint64_t subgroupId{0};
  uint64_t objectId{0};
  uint8_t priority{3};  // 0-7, lower = more urgent (matches HTTP/3 urgency)
  std::chrono::steady_clock::time_point deadline;
  std::unique_ptr<Payload> payload;
  bool endOfGroup{false};     // Last object in group
  bool endOfSubgroup{false};  // Last object in subgroup
  bool endOfTrack{false};     // Last object in track (FIN)

  // Size of payload for queue capacity tracking
  size_t payloadSize() const {
    return payload ? payload->computeChainDataLength() : 0;
  }
};

// Result of enqueue operation
enum class EnqueueResult {
  OK,           // Successfully enqueued
  QUEUE_FULL,   // Queue at capacity, caller should wait
  GROUP_EXPIRED // Group already expired, object dropped
};

// Statistics for monitoring
struct QueueStats {
  size_t objectCount{0};      // Number of objects in queue
  size_t byteCount{0};        // Total bytes queued
  size_t expiredGroups{0};    // Number of expired groups
  size_t droppedObjects{0};   // Objects dropped due to expiry
  size_t droppedBytes{0};     // Bytes dropped due to expiry
};

// =============================================================================
// StreamPriorityQueue - Per-stream priority queue with group expiry
// =============================================================================
//
// Each MoQ subgroup stream has its own queue. Group expiry is tracked locally
// but can be coordinated across streams via GroupExpiryTracker.
//

class GroupExpiryTracker;  // Forward declaration

class StreamPriorityQueue {
 public:
  // Configuration
  struct Config {
    size_t maxObjects{1000};        // Max objects in queue
    size_t maxBytes{16 * 1024 * 1024};  // Max bytes (16MB default)
    bool dropOnExpiry{true};        // Drop expired objects vs. send anyway
  };

  StreamPriorityQueue();
  explicit StreamPriorityQueue(Config config);
  ~StreamPriorityQueue();

  // Non-copyable, non-movable (has mutex)
  StreamPriorityQueue(const StreamPriorityQueue&) = delete;
  StreamPriorityQueue& operator=(const StreamPriorityQueue&) = delete;
  StreamPriorityQueue(StreamPriorityQueue&&) = delete;
  StreamPriorityQueue& operator=(StreamPriorityQueue&&) = delete;

  // --- Enqueue (Publisher side) ---

  // Enqueue an object with delivery deadline
  // Returns QUEUE_FULL if capacity exceeded (caller should awaitCredit)
  // Returns GROUP_EXPIRED if the group was already marked expired
  EnqueueResult enqueue(QueuedObject obj);

  // Convenience: enqueue with timeout duration instead of absolute deadline
  EnqueueResult enqueue(
      QueuedObject obj,
      std::chrono::milliseconds deliveryTimeout);

  // --- Dequeue (picoquic JIT side) ---

  // Dequeue the highest priority non-expired object
  // Returns nullopt if queue is empty or all remaining objects are expired
  // Automatically expires entire group if any object in it is past deadline
  std::optional<QueuedObject> dequeue();

  // Peek at the next object without removing it
  const QueuedObject* peek() const;

  // Dequeue up to maxBytes worth of objects (for batch sending)
  std::vector<QueuedObject> dequeueBatch(size_t maxBytes);

  // --- Group Expiry ---

  // Explicitly expire a group (all objects in this group will be dropped)
  void expireGroup(uint64_t groupId);

  // Check if a group is expired
  bool isGroupExpired(uint64_t groupId) const;

  // Set external group expiry tracker for cross-stream coordination
  void setGroupExpiryTracker(std::shared_ptr<GroupExpiryTracker> tracker);

  // --- Backpressure Signaling ---

  // Get a future that completes when the queue has room
  // Used by publisher to wait when queue is full
  SemiFuture<Unit> awaitCapacity();

  // Signal that capacity is available (called after dequeue)
  // This fulfills any pending awaitCapacity() futures
  void signalCapacityAvailable();

  // Get a future that completes when picoquic is ready for data
  // (prepare_to_send was called)
  SemiFuture<Unit> awaitReady();

  // Signal that picoquic is ready (called from prepare_to_send)
  void signalReady();

  // --- Queue State ---

  bool empty() const;
  bool isFull() const;
  size_t size() const;         // Object count
  size_t byteSize() const;     // Total bytes
  QueueStats stats() const;

  // --- Configuration ---

  void setMaxObjects(size_t max);
  void setMaxBytes(size_t max);

 private:
  // Internal entry with comparison for priority queue
  struct Entry {
    uint8_t priority;
    std::chrono::steady_clock::time_point deadline;
    uint64_t groupId;
    uint64_t objectId;
    size_t payloadSize;
    std::unique_ptr<Payload> payload;
    bool endOfGroup;
    bool endOfSubgroup;
    bool endOfTrack;

    // Greater-than for min-heap (lower priority number = higher priority)
    bool operator>(const Entry& other) const {
      if (priority != other.priority) {
        return priority > other.priority;
      }
      if (deadline != other.deadline) {
        return deadline > other.deadline;
      }
      return objectId > other.objectId;
    }
  };

  // Check and handle expiry for a group
  // Returns true if the group is/was expired
  bool checkAndExpireGroup(uint64_t groupId);

  // Drop all objects belonging to an expired group
  void dropExpiredGroupObjects(uint64_t groupId);

  // Rebuild the priority queue after dropping objects
  void rebuildQueue();

  mutable std::mutex mutex_;
  Config config_;

  // Priority queue (min-heap by priority/deadline)
  std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> queue_;

  // Track expired groups
  std::unordered_set<uint64_t> expiredGroups_;

  // Track earliest deadline per group for efficient expiry checking
  std::map<uint64_t, std::chrono::steady_clock::time_point> groupDeadlines_;

  // Statistics
  size_t byteCount_{0};
  size_t droppedObjects_{0};
  size_t droppedBytes_{0};

  // Backpressure signaling
  std::vector<Promise<Unit>> capacityWaiters_;
  std::vector<Promise<Unit>> readyWaiters_;

  // External group expiry coordination
  std::shared_ptr<GroupExpiryTracker> expiryTracker_;
};

// =============================================================================
// GroupExpiryTracker - Cross-stream group expiry coordination
// =============================================================================
//
// MoQT semantics: if any object in a group expires on any stream,
// the entire group should be considered expired across ALL streams.
// This tracker coordinates expiry across multiple StreamPriorityQueues.
//

class GroupExpiryTracker {
 public:
  GroupExpiryTracker() = default;

  // Mark a group as expired (called when any object in the group expires)
  void expireGroup(uint64_t groupId);

  // Check if a group is expired
  bool isExpired(uint64_t groupId) const;

  // Register callback for group expiry notifications
  using ExpiryCallback = std::function<void(uint64_t groupId)>;
  void onGroupExpired(ExpiryCallback callback);

  // Clear expired groups older than the given group ID
  // (Groups are monotonically increasing, so we can GC old entries)
  void clearExpiredBefore(uint64_t groupId);

  // Get count of tracked expired groups
  size_t expiredCount() const;

 private:
  mutable std::mutex mutex_;
  std::unordered_set<uint64_t> expiredGroups_;
  std::vector<ExpiryCallback> callbacks_;
};

// =============================================================================
// TrackPriorityQueue - Track-level queue managing multiple subgroup streams
// =============================================================================
//
// Higher-level abstraction that manages queues for all subgroups in a track.
// Coordinates group expiry across subgroups.
//

class TrackPriorityQueue {
 public:
  TrackPriorityQueue();
  explicit TrackPriorityQueue(StreamPriorityQueue::Config config);

  // Get or create queue for a subgroup
  StreamPriorityQueue& getSubgroupQueue(uint64_t subgroupId);

  // Enqueue to appropriate subgroup queue
  EnqueueResult enqueue(QueuedObject obj);
  EnqueueResult enqueue(
      QueuedObject obj,
      std::chrono::milliseconds deliveryTimeout);

  // Expire a group across all subgroup queues
  void expireGroup(uint64_t groupId);

  // Check if group is expired
  bool isGroupExpired(uint64_t groupId) const;

  // Get statistics across all subgroups
  QueueStats totalStats() const;

 private:
  StreamPriorityQueue::Config config_;
  std::shared_ptr<GroupExpiryTracker> expiryTracker_;
  std::map<uint64_t, std::unique_ptr<StreamPriorityQueue>> subgroupQueues_;
  mutable std::mutex mutex_;
};

} // namespace moxygen::compat
