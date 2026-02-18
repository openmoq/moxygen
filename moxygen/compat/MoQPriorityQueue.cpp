/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <moxygen/compat/MoQPriorityQueue.h>

namespace moxygen::compat {

// =============================================================================
// StreamPriorityQueue Implementation
// =============================================================================

StreamPriorityQueue::StreamPriorityQueue() : config_() {}

StreamPriorityQueue::StreamPriorityQueue(Config config)
    : config_(std::move(config)) {}

StreamPriorityQueue::~StreamPriorityQueue() {
  // Fulfill any pending waiters with exception to avoid hangs
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& promise : capacityWaiters_) {
    if (!promise.isFulfilled()) {
      promise.setException(std::make_exception_ptr(
          std::runtime_error("StreamPriorityQueue destroyed")));
    }
  }
  for (auto& promise : readyWaiters_) {
    if (!promise.isFulfilled()) {
      promise.setException(std::make_exception_ptr(
          std::runtime_error("StreamPriorityQueue destroyed")));
    }
  }
}

EnqueueResult StreamPriorityQueue::enqueue(QueuedObject obj) {
  std::lock_guard<std::mutex> lock(mutex_);

  // Check if group is already expired
  if (expiredGroups_.count(obj.groupId) > 0) {
    droppedObjects_++;
    droppedBytes_ += obj.payloadSize();
    return EnqueueResult::GROUP_EXPIRED;
  }

  // Check external tracker if available
  if (expiryTracker_ && expiryTracker_->isExpired(obj.groupId)) {
    expiredGroups_.insert(obj.groupId);
    droppedObjects_++;
    droppedBytes_ += obj.payloadSize();
    return EnqueueResult::GROUP_EXPIRED;
  }

  // Check capacity
  if (queue_.size() >= config_.maxObjects ||
      byteCount_ + obj.payloadSize() > config_.maxBytes) {
    return EnqueueResult::QUEUE_FULL;
  }

  // Track group deadline (earliest deadline in group)
  auto it = groupDeadlines_.find(obj.groupId);
  if (it == groupDeadlines_.end()) {
    groupDeadlines_[obj.groupId] = obj.deadline;
  } else if (obj.deadline < it->second) {
    it->second = obj.deadline;
  }

  // Create entry and enqueue
  size_t payloadSize = obj.payloadSize();
  Entry entry{
      obj.priority,
      obj.deadline,
      obj.groupId,
      obj.objectId,
      payloadSize,
      std::move(obj.payload),
      obj.endOfGroup,
      obj.endOfSubgroup,
      obj.endOfTrack};

  byteCount_ += payloadSize;
  queue_.push(std::move(entry));

  return EnqueueResult::OK;
}

EnqueueResult StreamPriorityQueue::enqueue(
    QueuedObject obj,
    std::chrono::milliseconds deliveryTimeout) {
  obj.deadline = std::chrono::steady_clock::now() + deliveryTimeout;
  return enqueue(std::move(obj));
}

std::optional<QueuedObject> StreamPriorityQueue::dequeue() {
  std::lock_guard<std::mutex> lock(mutex_);

  auto now = std::chrono::steady_clock::now();

  while (!queue_.empty()) {
    // Peek at top entry
    const Entry& top = queue_.top();

    // Check if group is expired
    if (expiredGroups_.count(top.groupId) > 0) {
      // Already marked expired, skip
      byteCount_ -= top.payloadSize;
      droppedObjects_++;
      droppedBytes_ += top.payloadSize;
      // Need to get a mutable reference to pop
      auto& mutableQueue = const_cast<
          std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>>&>(
          queue_);
      mutableQueue.pop();
      continue;
    }

    // Check if this object's deadline has passed
    if (config_.dropOnExpiry && top.deadline < now) {
      // Object expired - expire the ENTIRE group (MoQT semantics)
      uint64_t groupId = top.groupId;
      expiredGroups_.insert(groupId);

      // Notify external tracker
      if (expiryTracker_) {
        expiryTracker_->expireGroup(groupId);
      }

      byteCount_ -= top.payloadSize;
      droppedObjects_++;
      droppedBytes_ += top.payloadSize;

      // Pop and continue to check next
      auto& mutableQueue = const_cast<
          std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>>&>(
          queue_);
      mutableQueue.pop();
      continue;
    }

    // Valid object - dequeue it
    // We need to move out of the priority queue, which requires some gymnastics
    auto& mutableQueue = const_cast<
        std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>>&>(
        queue_);

    // Get mutable reference to top (safe because we hold the lock)
    Entry& mutableTop =
        const_cast<Entry&>(static_cast<const Entry&>(mutableQueue.top()));

    QueuedObject result;
    result.groupId = mutableTop.groupId;
    result.objectId = mutableTop.objectId;
    result.priority = mutableTop.priority;
    result.deadline = mutableTop.deadline;
    result.payload = std::move(mutableTop.payload);
    result.endOfGroup = mutableTop.endOfGroup;
    result.endOfSubgroup = mutableTop.endOfSubgroup;
    result.endOfTrack = mutableTop.endOfTrack;

    byteCount_ -= mutableTop.payloadSize;
    mutableQueue.pop();

    // Signal capacity available if we were full
    if (!capacityWaiters_.empty()) {
      for (auto& promise : capacityWaiters_) {
        if (!promise.isFulfilled()) {
          promise.setValue(unit);
        }
      }
      capacityWaiters_.clear();
    }

    return result;
  }

  return std::nullopt;
}

const QueuedObject* StreamPriorityQueue::peek() const {
  // Note: This is a simplified peek that doesn't check expiry
  // Full implementation would need to handle expired objects
  return nullptr;  // Priority queue doesn't support efficient peek-with-filter
}

std::vector<QueuedObject> StreamPriorityQueue::dequeueBatch(size_t maxBytes) {
  std::vector<QueuedObject> batch;
  size_t batchBytes = 0;

  while (batchBytes < maxBytes) {
    auto obj = dequeue();
    if (!obj) {
      break;
    }
    batchBytes += obj->payloadSize();
    batch.push_back(std::move(*obj));
  }

  return batch;
}

void StreamPriorityQueue::expireGroup(uint64_t groupId) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (expiredGroups_.count(groupId) > 0) {
    return;  // Already expired
  }

  expiredGroups_.insert(groupId);
  groupDeadlines_.erase(groupId);

  // Notify external tracker
  if (expiryTracker_) {
    expiryTracker_->expireGroup(groupId);
  }

  // Note: We don't rebuild the queue here - expired objects will be
  // dropped lazily during dequeue(). This is more efficient than
  // scanning the entire queue.
}

bool StreamPriorityQueue::isGroupExpired(uint64_t groupId) const {
  std::lock_guard<std::mutex> lock(mutex_);

  if (expiredGroups_.count(groupId) > 0) {
    return true;
  }

  if (expiryTracker_ && expiryTracker_->isExpired(groupId)) {
    return true;
  }

  return false;
}

void StreamPriorityQueue::setGroupExpiryTracker(
    std::shared_ptr<GroupExpiryTracker> tracker) {
  std::lock_guard<std::mutex> lock(mutex_);
  expiryTracker_ = std::move(tracker);
}

SemiFuture<Unit> StreamPriorityQueue::awaitCapacity() {
  std::lock_guard<std::mutex> lock(mutex_);

  // If we have capacity, return immediately ready future
  if (queue_.size() < config_.maxObjects &&
      byteCount_ < config_.maxBytes) {
    return SemiFuture<Unit>(unit);
  }

  // Otherwise, create a promise and return its future
  capacityWaiters_.emplace_back();
  return capacityWaiters_.back().getSemiFuture();
}

void StreamPriorityQueue::signalCapacityAvailable() {
  std::lock_guard<std::mutex> lock(mutex_);

  for (auto& promise : capacityWaiters_) {
    if (!promise.isFulfilled()) {
      promise.setValue(unit);
    }
  }
  capacityWaiters_.clear();
}

SemiFuture<Unit> StreamPriorityQueue::awaitReady() {
  std::lock_guard<std::mutex> lock(mutex_);

  // Create a promise and return its future
  // This will be fulfilled when signalReady() is called (from prepare_to_send)
  readyWaiters_.emplace_back();
  return readyWaiters_.back().getSemiFuture();
}

void StreamPriorityQueue::signalReady() {
  std::lock_guard<std::mutex> lock(mutex_);

  for (auto& promise : readyWaiters_) {
    if (!promise.isFulfilled()) {
      promise.setValue(unit);
    }
  }
  readyWaiters_.clear();
}

bool StreamPriorityQueue::empty() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return queue_.empty();
}

bool StreamPriorityQueue::isFull() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return queue_.size() >= config_.maxObjects ||
         byteCount_ >= config_.maxBytes;
}

size_t StreamPriorityQueue::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return queue_.size();
}

size_t StreamPriorityQueue::byteSize() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return byteCount_;
}

QueueStats StreamPriorityQueue::stats() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return QueueStats{
      queue_.size(),
      byteCount_,
      expiredGroups_.size(),
      droppedObjects_,
      droppedBytes_};
}

void StreamPriorityQueue::setMaxObjects(size_t max) {
  std::lock_guard<std::mutex> lock(mutex_);
  config_.maxObjects = max;
}

void StreamPriorityQueue::setMaxBytes(size_t max) {
  std::lock_guard<std::mutex> lock(mutex_);
  config_.maxBytes = max;
}

bool StreamPriorityQueue::checkAndExpireGroup(uint64_t groupId) {
  // Must be called with mutex_ held
  if (expiredGroups_.count(groupId) > 0) {
    return true;
  }

  auto it = groupDeadlines_.find(groupId);
  if (it != groupDeadlines_.end()) {
    auto now = std::chrono::steady_clock::now();
    if (it->second < now) {
      expiredGroups_.insert(groupId);
      groupDeadlines_.erase(it);

      if (expiryTracker_) {
        expiryTracker_->expireGroup(groupId);
      }
      return true;
    }
  }

  return false;
}

void StreamPriorityQueue::dropExpiredGroupObjects(uint64_t groupId) {
  // This would require rebuilding the priority queue
  // Instead, we handle this lazily in dequeue()
}

void StreamPriorityQueue::rebuildQueue() {
  // Not implemented - we use lazy expiry in dequeue() instead
}

// =============================================================================
// GroupExpiryTracker Implementation
// =============================================================================

void GroupExpiryTracker::expireGroup(uint64_t groupId) {
  std::vector<ExpiryCallback> callbacksCopy;

  {
    std::lock_guard<std::mutex> lock(mutex_);

    if (expiredGroups_.count(groupId) > 0) {
      return;  // Already expired
    }

    expiredGroups_.insert(groupId);
    callbacksCopy = callbacks_;  // Copy for callback invocation outside lock
  }

  // Invoke callbacks outside lock to prevent deadlocks
  for (auto& callback : callbacksCopy) {
    try {
      callback(groupId);
    } catch (...) {
      // Swallow callback exceptions
    }
  }
}

bool GroupExpiryTracker::isExpired(uint64_t groupId) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return expiredGroups_.count(groupId) > 0;
}

void GroupExpiryTracker::onGroupExpired(ExpiryCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  callbacks_.push_back(std::move(callback));
}

void GroupExpiryTracker::clearExpiredBefore(uint64_t groupId) {
  std::lock_guard<std::mutex> lock(mutex_);

  // Remove all expired group IDs less than groupId
  for (auto it = expiredGroups_.begin(); it != expiredGroups_.end();) {
    if (*it < groupId) {
      it = expiredGroups_.erase(it);
    } else {
      ++it;
    }
  }
}

size_t GroupExpiryTracker::expiredCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return expiredGroups_.size();
}

// =============================================================================
// TrackPriorityQueue Implementation
// =============================================================================

TrackPriorityQueue::TrackPriorityQueue()
    : config_(), expiryTracker_(std::make_shared<GroupExpiryTracker>()) {}

TrackPriorityQueue::TrackPriorityQueue(StreamPriorityQueue::Config config)
    : config_(std::move(config)),
      expiryTracker_(std::make_shared<GroupExpiryTracker>()) {}

StreamPriorityQueue& TrackPriorityQueue::getSubgroupQueue(uint64_t subgroupId) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = subgroupQueues_.find(subgroupId);
  if (it == subgroupQueues_.end()) {
    auto queue = std::make_unique<StreamPriorityQueue>(config_);
    queue->setGroupExpiryTracker(expiryTracker_);
    it = subgroupQueues_.emplace(subgroupId, std::move(queue)).first;
  }

  return *it->second;
}

EnqueueResult TrackPriorityQueue::enqueue(QueuedObject obj) {
  return getSubgroupQueue(obj.subgroupId).enqueue(std::move(obj));
}

EnqueueResult TrackPriorityQueue::enqueue(
    QueuedObject obj,
    std::chrono::milliseconds deliveryTimeout) {
  return getSubgroupQueue(obj.subgroupId)
      .enqueue(std::move(obj), deliveryTimeout);
}

void TrackPriorityQueue::expireGroup(uint64_t groupId) {
  // Expire in the tracker (will propagate to all queues)
  expiryTracker_->expireGroup(groupId);

  // Also explicitly expire in each queue for immediate effect
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& [subgroupId, queue] : subgroupQueues_) {
    queue->expireGroup(groupId);
  }
}

bool TrackPriorityQueue::isGroupExpired(uint64_t groupId) const {
  return expiryTracker_->isExpired(groupId);
}

QueueStats TrackPriorityQueue::totalStats() const {
  std::lock_guard<std::mutex> lock(mutex_);

  QueueStats total;
  for (auto& [subgroupId, queue] : subgroupQueues_) {
    auto s = queue->stats();
    total.objectCount += s.objectCount;
    total.byteCount += s.byteCount;
    total.expiredGroups += s.expiredGroups;
    total.droppedObjects += s.droppedObjects;
    total.droppedBytes += s.droppedBytes;
  }

  return total;
}

} // namespace moxygen::compat
