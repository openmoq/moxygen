/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <moxygen/compat/Config.h>

#if MOXYGEN_USE_FOLLY
#include <folly/container/F14Map.h>
#include <folly/container/F14Set.h>
#elif MOXYGEN_USE_ABSEIL
#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <absl/container/node_hash_map.h>
#include <absl/container/node_hash_set.h>
#endif

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace moxygen::compat {

#if MOXYGEN_USE_FOLLY

// Folly mode: use high-performance F14 containers
template <
    typename Key,
    typename Value,
    typename Hash = std::hash<Key>,
    typename KeyEqual = std::equal_to<Key>>
using FastMap = folly::F14FastMap<Key, Value, Hash, KeyEqual>;

template <
    typename Key,
    typename Hash = std::hash<Key>,
    typename KeyEqual = std::equal_to<Key>>
using FastSet = folly::F14FastSet<Key, Hash, KeyEqual>;

template <
    typename Key,
    typename Value,
    typename Hash = std::hash<Key>,
    typename KeyEqual = std::equal_to<Key>>
using NodeMap = folly::F14NodeMap<Key, Value, Hash, KeyEqual>;

template <
    typename Key,
    typename Hash = std::hash<Key>,
    typename KeyEqual = std::equal_to<Key>>
using NodeSet = folly::F14NodeSet<Key, Hash, KeyEqual>;

#elif MOXYGEN_USE_ABSEIL

// =============================================================================
// Abseil mode: use Swiss table containers
// =============================================================================
// Swiss table provides SIMD-accelerated probing (~2x faster than std::unordered_map).
// This is the same algorithm used by Folly F14.

template <
    typename Key,
    typename Value,
    typename Hash = std::hash<Key>,
    typename KeyEqual = std::equal_to<Key>>
using FastMap = absl::flat_hash_map<Key, Value, Hash, KeyEqual>;

template <
    typename Key,
    typename Hash = std::hash<Key>,
    typename KeyEqual = std::equal_to<Key>>
using FastSet = absl::flat_hash_set<Key, Hash, KeyEqual>;

template <
    typename Key,
    typename Value,
    typename Hash = std::hash<Key>,
    typename KeyEqual = std::equal_to<Key>>
using NodeMap = absl::node_hash_map<Key, Value, Hash, KeyEqual>;

template <
    typename Key,
    typename Hash = std::hash<Key>,
    typename KeyEqual = std::equal_to<Key>>
using NodeSet = absl::node_hash_set<Key, Hash, KeyEqual>;

#else // !MOXYGEN_USE_FOLLY && !MOXYGEN_USE_ABSEIL

// =============================================================================
// Robin Hood Hash Map
// =============================================================================
// Open-addressing hash table with robin hood hashing.
// ~1.5-2x faster than std::unordered_map for lookups.
//
// Key features:
// - Linear probing with displacement tracking
// - Power-of-2 sizing for fast modulo (bitmasking)
// - Backward shift deletion (no tombstones)
// - Cache-friendly flat memory layout
//
// Robin hood invariant: When inserting, if we find an element with smaller
// displacement (probe sequence length), we swap and continue inserting the
// displaced element. This keeps variance low and lookup fast.
//
// References:
// - Pedro Celis, "Robin Hood Hashing", PhD thesis, University of Waterloo, 1986
//   https://cs.uwaterloo.ca/research/tr/1986/CS-86-14.pdf
// - E. Amble and D.E. Knuth, "Ordered hash tables", The Computer Journal, 1974
// - Martin Ankerl, "Robin Hood Hashing should be your default Hash Table"
//   https://martin.ankerl.com/2019/04/01/hashmap-benchmarks-01-overview/
// =============================================================================

namespace detail {

// Slot states
enum class SlotState : uint8_t {
  Empty = 0,
  Occupied = 1,
};

// Compute next power of 2 >= n
inline size_t nextPowerOf2(size_t n) noexcept {
  if (n == 0) return 1;
  --n;
  n |= n >> 1;
  n |= n >> 2;
  n |= n >> 4;
  n |= n >> 8;
  n |= n >> 16;
  if constexpr (sizeof(size_t) == 8) {
    n |= n >> 32;
  }
  return n + 1;
}

} // namespace detail

template <
    typename Key,
    typename Value,
    typename Hash = std::hash<Key>,
    typename KeyEqual = std::equal_to<Key>>
class RobinHoodMap {
 public:
  using key_type = Key;
  using mapped_type = Value;
  using value_type = std::pair<const Key, Value>;
  using size_type = size_t;
  using difference_type = std::ptrdiff_t;
  using hasher = Hash;
  using key_equal = KeyEqual;

 private:
  // Internal storage uses mutable key for moves during robin hood
  struct Slot {
    alignas(value_type) char storage[sizeof(value_type)];
    detail::SlotState state{detail::SlotState::Empty};
    uint8_t displacement{0};  // Probe sequence length

    value_type* ptr() noexcept {
      return reinterpret_cast<value_type*>(storage);
    }
    const value_type* ptr() const noexcept {
      return reinterpret_cast<const value_type*>(storage);
    }

    bool empty() const noexcept {
      return state == detail::SlotState::Empty;
    }
    bool occupied() const noexcept {
      return state == detail::SlotState::Occupied;
    }
  };

 public:
  // Iterator
  class iterator {
   public:
    using value_type = std::pair<const Key, Value>;
    using reference = value_type&;
    using pointer = value_type*;
    using difference_type = std::ptrdiff_t;
    using iterator_category = std::forward_iterator_tag;

    iterator() = default;
    iterator(Slot* slot, Slot* end) : slot_(slot), end_(end) {
      advanceToOccupied();
    }

    reference operator*() const { return *slot_->ptr(); }
    pointer operator->() const { return slot_->ptr(); }

    iterator& operator++() {
      ++slot_;
      advanceToOccupied();
      return *this;
    }

    iterator operator++(int) {
      iterator tmp = *this;
      ++(*this);
      return tmp;
    }

    bool operator==(const iterator& other) const { return slot_ == other.slot_; }
    bool operator!=(const iterator& other) const { return slot_ != other.slot_; }

   private:
    void advanceToOccupied() {
      while (slot_ != end_ && slot_->empty()) {
        ++slot_;
      }
    }

    Slot* slot_{nullptr};
    Slot* end_{nullptr};
    friend class RobinHoodMap;
  };

  class const_iterator {
   public:
    using value_type = const std::pair<const Key, Value>;
    using reference = const value_type&;
    using pointer = const value_type*;
    using difference_type = std::ptrdiff_t;
    using iterator_category = std::forward_iterator_tag;

    const_iterator() = default;
    const_iterator(const Slot* slot, const Slot* end) : slot_(slot), end_(end) {
      advanceToOccupied();
    }
    const_iterator(iterator it) : slot_(it.slot_), end_(it.end_) {}

    reference operator*() const { return *slot_->ptr(); }
    pointer operator->() const { return slot_->ptr(); }

    const_iterator& operator++() {
      ++slot_;
      advanceToOccupied();
      return *this;
    }

    const_iterator operator++(int) {
      const_iterator tmp = *this;
      ++(*this);
      return tmp;
    }

    bool operator==(const const_iterator& other) const { return slot_ == other.slot_; }
    bool operator!=(const const_iterator& other) const { return slot_ != other.slot_; }

   private:
    void advanceToOccupied() {
      while (slot_ != end_ && slot_->empty()) {
        ++slot_;
      }
    }

    const Slot* slot_{nullptr};
    const Slot* end_{nullptr};
  };

  // Constructors
  RobinHoodMap() = default;

  explicit RobinHoodMap(size_type initialCapacity,
                        const Hash& hash = Hash(),
                        const KeyEqual& equal = KeyEqual())
      : hash_(hash), equal_(equal) {
    if (initialCapacity > 0) {
      reserve(initialCapacity);
    }
  }

  RobinHoodMap(std::initializer_list<value_type> init,
               const Hash& hash = Hash(),
               const KeyEqual& equal = KeyEqual())
      : hash_(hash), equal_(equal) {
    reserve(init.size());
    for (const auto& p : init) {
      insert(p);
    }
  }

  ~RobinHoodMap() {
    clear();
  }

  // Move constructor
  RobinHoodMap(RobinHoodMap&& other) noexcept
      : slots_(std::move(other.slots_)),
        size_(other.size_),
        capacity_(other.capacity_),
        mask_(other.mask_),
        hash_(std::move(other.hash_)),
        equal_(std::move(other.equal_)) {
    other.size_ = 0;
    other.capacity_ = 0;
    other.mask_ = 0;
  }

  // Move assignment
  RobinHoodMap& operator=(RobinHoodMap&& other) noexcept {
    if (this != &other) {
      clear();
      slots_ = std::move(other.slots_);
      size_ = other.size_;
      capacity_ = other.capacity_;
      mask_ = other.mask_;
      hash_ = std::move(other.hash_);
      equal_ = std::move(other.equal_);
      other.size_ = 0;
      other.capacity_ = 0;
      other.mask_ = 0;
    }
    return *this;
  }

  // Copy constructor
  RobinHoodMap(const RobinHoodMap& other)
      : hash_(other.hash_), equal_(other.equal_) {
    if (other.size_ > 0) {
      reserve(other.size_);
      for (const auto& p : other) {
        insert(p);
      }
    }
  }

  // Copy assignment
  RobinHoodMap& operator=(const RobinHoodMap& other) {
    if (this != &other) {
      clear();
      hash_ = other.hash_;
      equal_ = other.equal_;
      if (other.size_ > 0) {
        reserve(other.size_);
        for (const auto& p : other) {
          insert(p);
        }
      }
    }
    return *this;
  }

  // Capacity
  bool empty() const noexcept { return size_ == 0; }
  size_type size() const noexcept { return size_; }
  size_type max_size() const noexcept { return SIZE_MAX / sizeof(Slot); }

  // Iterators
  iterator begin() noexcept {
    return iterator(slots_.data(), slots_.data() + capacity_);
  }
  iterator end() noexcept {
    return iterator(slots_.data() + capacity_, slots_.data() + capacity_);
  }
  const_iterator begin() const noexcept {
    return const_iterator(slots_.data(), slots_.data() + capacity_);
  }
  const_iterator end() const noexcept {
    return const_iterator(slots_.data() + capacity_, slots_.data() + capacity_);
  }
  const_iterator cbegin() const noexcept { return begin(); }
  const_iterator cend() const noexcept { return end(); }

  // Lookup
  iterator find(const Key& key) {
    if (capacity_ == 0) return end();

    size_t hash = hash_(key);
    size_t idx = hash & mask_;
    uint8_t disp = 0;

    while (true) {
      Slot& slot = slots_[idx];
      if (slot.empty()) {
        return end();
      }
      if (slot.displacement < disp) {
        // Robin hood: element would have been placed here if it existed
        return end();
      }
      if (slot.displacement == disp && equal_(slot.ptr()->first, key)) {
        return iterator(&slot, slots_.data() + capacity_);
      }
      idx = (idx + 1) & mask_;
      ++disp;
    }
  }

  const_iterator find(const Key& key) const {
    return const_cast<RobinHoodMap*>(this)->find(key);
  }

  bool contains(const Key& key) const {
    return find(key) != end();
  }

  size_type count(const Key& key) const {
    return contains(key) ? 1 : 0;
  }

  // Element access
  Value& operator[](const Key& key) {
    auto it = find(key);
    if (it != end()) {
      return it->second;
    }
    return insert({key, Value()}).first->second;
  }

  Value& operator[](Key&& key) {
    auto it = find(key);
    if (it != end()) {
      return it->second;
    }
    return insert({std::move(key), Value()}).first->second;
  }

  Value& at(const Key& key) {
    auto it = find(key);
    if (it == end()) {
      throw std::out_of_range("RobinHoodMap::at");
    }
    return it->second;
  }

  const Value& at(const Key& key) const {
    auto it = find(key);
    if (it == end()) {
      throw std::out_of_range("RobinHoodMap::at");
    }
    return it->second;
  }

  // Modifiers
  std::pair<iterator, bool> insert(const value_type& value) {
    return insertImpl(value.first, value.second);
  }

  std::pair<iterator, bool> insert(value_type&& value) {
    return insertImpl(std::move(const_cast<Key&>(value.first)), std::move(value.second));
  }

  template <typename... Args>
  std::pair<iterator, bool> emplace(Args&&... args) {
    // Construct temporary to extract key
    value_type temp(std::forward<Args>(args)...);
    return insertImpl(std::move(const_cast<Key&>(temp.first)), std::move(temp.second));
  }

  template <typename... Args>
  std::pair<iterator, bool> try_emplace(const Key& key, Args&&... args) {
    auto it = find(key);
    if (it != end()) {
      return {it, false};
    }
    return insertImpl(key, Value(std::forward<Args>(args)...));
  }

  template <typename... Args>
  std::pair<iterator, bool> try_emplace(Key&& key, Args&&... args) {
    auto it = find(key);
    if (it != end()) {
      return {it, false};
    }
    return insertImpl(std::move(key), Value(std::forward<Args>(args)...));
  }

  size_type erase(const Key& key) {
    auto it = find(key);
    if (it == end()) {
      return 0;
    }
    eraseImpl(it);
    return 1;
  }

  iterator erase(iterator pos) {
    if (pos == end()) {
      return end();
    }
    size_t idx = pos.slot_ - slots_.data();
    eraseImpl(pos);
    // Return iterator to next element
    return iterator(slots_.data() + idx, slots_.data() + capacity_);
  }

  void clear() {
    for (size_t i = 0; i < capacity_; ++i) {
      if (slots_[i].occupied()) {
        slots_[i].ptr()->~value_type();
        slots_[i].state = detail::SlotState::Empty;
      }
    }
    size_ = 0;
  }

  void reserve(size_type count) {
    size_t needed = static_cast<size_t>(count / kMaxLoadFactor) + 1;
    if (needed > capacity_) {
      rehash(detail::nextPowerOf2(needed));
    }
  }

 private:
  static constexpr float kMaxLoadFactor = 0.875f;  // 7/8
  static constexpr size_t kMinCapacity = 8;

  template <typename K, typename V>
  std::pair<iterator, bool> insertImpl(K&& key, V&& value) {
    // Check if we need to grow
    if (size_ >= capacity_ * kMaxLoadFactor) {
      size_t newCap = capacity_ == 0 ? kMinCapacity : capacity_ * 2;
      rehash(newCap);
    }

    size_t hash = hash_(key);
    size_t idx = hash & mask_;
    uint8_t disp = 0;

    Key insertKey = std::forward<K>(key);
    Value insertValue = std::forward<V>(value);

    while (true) {
      Slot& slot = slots_[idx];

      if (slot.empty()) {
        // Found empty slot - insert here
        new (slot.storage) value_type(std::move(insertKey), std::move(insertValue));
        slot.state = detail::SlotState::Occupied;
        slot.displacement = disp;
        ++size_;
        return {iterator(&slot, slots_.data() + capacity_), true};
      }

      if (slot.displacement == disp && equal_(slot.ptr()->first, insertKey)) {
        // Key already exists
        return {iterator(&slot, slots_.data() + capacity_), false};
      }

      if (slot.displacement < disp) {
        // Robin hood: steal from the rich (low displacement), give to the poor (high displacement)
        std::swap(insertKey, const_cast<Key&>(slot.ptr()->first));
        std::swap(insertValue, slot.ptr()->second);
        std::swap(disp, slot.displacement);
      }

      idx = (idx + 1) & mask_;
      ++disp;
    }
  }

  void eraseImpl(iterator pos) {
    size_t idx = pos.slot_ - slots_.data();

    // Destroy element
    slots_[idx].ptr()->~value_type();
    slots_[idx].state = detail::SlotState::Empty;
    --size_;

    // Backward shift deletion: move subsequent elements back to fill gap
    size_t next = (idx + 1) & mask_;
    while (!slots_[next].empty() && slots_[next].displacement > 0) {
      // Move element back
      new (slots_[idx].storage) value_type(std::move(*slots_[next].ptr()));
      slots_[idx].state = detail::SlotState::Occupied;
      slots_[idx].displacement = slots_[next].displacement - 1;

      slots_[next].ptr()->~value_type();
      slots_[next].state = detail::SlotState::Empty;

      idx = next;
      next = (next + 1) & mask_;
    }
  }

  void rehash(size_t newCapacity) {
    std::vector<Slot> oldSlots = std::move(slots_);
    size_t oldCapacity = capacity_;

    capacity_ = newCapacity;
    mask_ = capacity_ - 1;
    slots_.resize(capacity_);
    size_ = 0;

    // Reinsert all elements
    for (size_t i = 0; i < oldCapacity; ++i) {
      if (oldSlots[i].occupied()) {
        insertImpl(
            std::move(const_cast<Key&>(oldSlots[i].ptr()->first)),
            std::move(oldSlots[i].ptr()->second));
        oldSlots[i].ptr()->~value_type();
      }
    }
  }

  std::vector<Slot> slots_;
  size_t size_{0};
  size_t capacity_{0};
  size_t mask_{0};
  Hash hash_;
  KeyEqual equal_;
};

// =============================================================================
// Robin Hood Hash Set
// =============================================================================

template <
    typename Key,
    typename Hash = std::hash<Key>,
    typename KeyEqual = std::equal_to<Key>>
class RobinHoodSet {
 public:
  using key_type = Key;
  using value_type = Key;
  using size_type = size_t;
  using difference_type = std::ptrdiff_t;
  using hasher = Hash;
  using key_equal = KeyEqual;

 private:
  struct Slot {
    alignas(Key) char storage[sizeof(Key)];
    detail::SlotState state{detail::SlotState::Empty};
    uint8_t displacement{0};

    Key* ptr() noexcept {
      return reinterpret_cast<Key*>(storage);
    }
    const Key* ptr() const noexcept {
      return reinterpret_cast<const Key*>(storage);
    }

    bool empty() const noexcept {
      return state == detail::SlotState::Empty;
    }
    bool occupied() const noexcept {
      return state == detail::SlotState::Occupied;
    }
  };

 public:
  class iterator {
   public:
    using value_type = const Key;
    using reference = const Key&;
    using pointer = const Key*;
    using difference_type = std::ptrdiff_t;
    using iterator_category = std::forward_iterator_tag;

    iterator() = default;
    iterator(Slot* slot, Slot* end) : slot_(slot), end_(end) {
      advanceToOccupied();
    }

    reference operator*() const { return *slot_->ptr(); }
    pointer operator->() const { return slot_->ptr(); }

    iterator& operator++() {
      ++slot_;
      advanceToOccupied();
      return *this;
    }

    iterator operator++(int) {
      iterator tmp = *this;
      ++(*this);
      return tmp;
    }

    bool operator==(const iterator& other) const { return slot_ == other.slot_; }
    bool operator!=(const iterator& other) const { return slot_ != other.slot_; }

   private:
    void advanceToOccupied() {
      while (slot_ != end_ && slot_->empty()) {
        ++slot_;
      }
    }

    Slot* slot_{nullptr};
    Slot* end_{nullptr};
    friend class RobinHoodSet;
  };

  using const_iterator = iterator;

  // Constructors
  RobinHoodSet() = default;

  explicit RobinHoodSet(size_type initialCapacity,
                        const Hash& hash = Hash(),
                        const KeyEqual& equal = KeyEqual())
      : hash_(hash), equal_(equal) {
    if (initialCapacity > 0) {
      reserve(initialCapacity);
    }
  }

  RobinHoodSet(std::initializer_list<Key> init,
               const Hash& hash = Hash(),
               const KeyEqual& equal = KeyEqual())
      : hash_(hash), equal_(equal) {
    reserve(init.size());
    for (const auto& k : init) {
      insert(k);
    }
  }

  ~RobinHoodSet() {
    clear();
  }

  // Move constructor/assignment
  RobinHoodSet(RobinHoodSet&& other) noexcept
      : slots_(std::move(other.slots_)),
        size_(other.size_),
        capacity_(other.capacity_),
        mask_(other.mask_),
        hash_(std::move(other.hash_)),
        equal_(std::move(other.equal_)) {
    other.size_ = 0;
    other.capacity_ = 0;
    other.mask_ = 0;
  }

  RobinHoodSet& operator=(RobinHoodSet&& other) noexcept {
    if (this != &other) {
      clear();
      slots_ = std::move(other.slots_);
      size_ = other.size_;
      capacity_ = other.capacity_;
      mask_ = other.mask_;
      hash_ = std::move(other.hash_);
      equal_ = std::move(other.equal_);
      other.size_ = 0;
      other.capacity_ = 0;
      other.mask_ = 0;
    }
    return *this;
  }

  // Copy constructor/assignment
  RobinHoodSet(const RobinHoodSet& other)
      : hash_(other.hash_), equal_(other.equal_) {
    if (other.size_ > 0) {
      reserve(other.size_);
      for (const auto& k : other) {
        insert(k);
      }
    }
  }

  RobinHoodSet& operator=(const RobinHoodSet& other) {
    if (this != &other) {
      clear();
      hash_ = other.hash_;
      equal_ = other.equal_;
      if (other.size_ > 0) {
        reserve(other.size_);
        for (const auto& k : other) {
          insert(k);
        }
      }
    }
    return *this;
  }

  // Capacity
  bool empty() const noexcept { return size_ == 0; }
  size_type size() const noexcept { return size_; }

  // Iterators
  iterator begin() noexcept {
    return iterator(slots_.data(), slots_.data() + capacity_);
  }
  iterator end() noexcept {
    return iterator(slots_.data() + capacity_, slots_.data() + capacity_);
  }
  const_iterator begin() const noexcept {
    return const_cast<RobinHoodSet*>(this)->begin();
  }
  const_iterator end() const noexcept {
    return const_cast<RobinHoodSet*>(this)->end();
  }
  const_iterator cbegin() const noexcept { return begin(); }
  const_iterator cend() const noexcept { return end(); }

  // Lookup
  iterator find(const Key& key) {
    if (capacity_ == 0) return end();

    size_t hash = hash_(key);
    size_t idx = hash & mask_;
    uint8_t disp = 0;

    while (true) {
      Slot& slot = slots_[idx];
      if (slot.empty()) {
        return end();
      }
      if (slot.displacement < disp) {
        return end();
      }
      if (slot.displacement == disp && equal_(*slot.ptr(), key)) {
        return iterator(&slot, slots_.data() + capacity_);
      }
      idx = (idx + 1) & mask_;
      ++disp;
    }
  }

  const_iterator find(const Key& key) const {
    return const_cast<RobinHoodSet*>(this)->find(key);
  }

  bool contains(const Key& key) const {
    return find(key) != end();
  }

  size_type count(const Key& key) const {
    return contains(key) ? 1 : 0;
  }

  // Modifiers
  std::pair<iterator, bool> insert(const Key& key) {
    return insertImpl(key);
  }

  std::pair<iterator, bool> insert(Key&& key) {
    return insertImpl(std::move(key));
  }

  template <typename... Args>
  std::pair<iterator, bool> emplace(Args&&... args) {
    return insertImpl(Key(std::forward<Args>(args)...));
  }

  size_type erase(const Key& key) {
    auto it = find(key);
    if (it == end()) {
      return 0;
    }
    eraseImpl(it);
    return 1;
  }

  iterator erase(iterator pos) {
    if (pos == end()) {
      return end();
    }
    size_t idx = pos.slot_ - slots_.data();
    eraseImpl(pos);
    return iterator(slots_.data() + idx, slots_.data() + capacity_);
  }

  void clear() {
    for (size_t i = 0; i < capacity_; ++i) {
      if (slots_[i].occupied()) {
        slots_[i].ptr()->~Key();
        slots_[i].state = detail::SlotState::Empty;
      }
    }
    size_ = 0;
  }

  void reserve(size_type count) {
    size_t needed = static_cast<size_t>(count / kMaxLoadFactor) + 1;
    if (needed > capacity_) {
      rehash(detail::nextPowerOf2(needed));
    }
  }

 private:
  static constexpr float kMaxLoadFactor = 0.875f;
  static constexpr size_t kMinCapacity = 8;

  template <typename K>
  std::pair<iterator, bool> insertImpl(K&& key) {
    if (size_ >= capacity_ * kMaxLoadFactor) {
      size_t newCap = capacity_ == 0 ? kMinCapacity : capacity_ * 2;
      rehash(newCap);
    }

    size_t hash = hash_(key);
    size_t idx = hash & mask_;
    uint8_t disp = 0;

    Key insertKey = std::forward<K>(key);

    while (true) {
      Slot& slot = slots_[idx];

      if (slot.empty()) {
        new (slot.storage) Key(std::move(insertKey));
        slot.state = detail::SlotState::Occupied;
        slot.displacement = disp;
        ++size_;
        return {iterator(&slot, slots_.data() + capacity_), true};
      }

      if (slot.displacement == disp && equal_(*slot.ptr(), insertKey)) {
        return {iterator(&slot, slots_.data() + capacity_), false};
      }

      if (slot.displacement < disp) {
        std::swap(insertKey, *slot.ptr());
        std::swap(disp, slot.displacement);
      }

      idx = (idx + 1) & mask_;
      ++disp;
    }
  }

  void eraseImpl(iterator pos) {
    size_t idx = pos.slot_ - slots_.data();

    slots_[idx].ptr()->~Key();
    slots_[idx].state = detail::SlotState::Empty;
    --size_;

    size_t next = (idx + 1) & mask_;
    while (!slots_[next].empty() && slots_[next].displacement > 0) {
      new (slots_[idx].storage) Key(std::move(*slots_[next].ptr()));
      slots_[idx].state = detail::SlotState::Occupied;
      slots_[idx].displacement = slots_[next].displacement - 1;

      slots_[next].ptr()->~Key();
      slots_[next].state = detail::SlotState::Empty;

      idx = next;
      next = (next + 1) & mask_;
    }
  }

  void rehash(size_t newCapacity) {
    std::vector<Slot> oldSlots = std::move(slots_);
    size_t oldCapacity = capacity_;

    capacity_ = newCapacity;
    mask_ = capacity_ - 1;
    slots_.resize(capacity_);
    size_ = 0;

    for (size_t i = 0; i < oldCapacity; ++i) {
      if (oldSlots[i].occupied()) {
        insertImpl(std::move(*oldSlots[i].ptr()));
        oldSlots[i].ptr()->~Key();
      }
    }
  }

  std::vector<Slot> slots_;
  size_t size_{0};
  size_t capacity_{0};
  size_t mask_{0};
  Hash hash_;
  KeyEqual equal_;
};

// =============================================================================
// Type aliases for std mode
// =============================================================================

// FastMap/FastSet use Robin Hood for performance
template <
    typename Key,
    typename Value,
    typename Hash = std::hash<Key>,
    typename KeyEqual = std::equal_to<Key>>
using FastMap = RobinHoodMap<Key, Value, Hash, KeyEqual>;

template <
    typename Key,
    typename Hash = std::hash<Key>,
    typename KeyEqual = std::equal_to<Key>>
using FastSet = RobinHoodSet<Key, Hash, KeyEqual>;

// NodeMap/NodeSet use std::unordered for stable references
// (Robin Hood doesn't provide stable references on rehash)
template <
    typename Key,
    typename Value,
    typename Hash = std::hash<Key>,
    typename KeyEqual = std::equal_to<Key>>
using NodeMap = std::unordered_map<Key, Value, Hash, KeyEqual>;

template <
    typename Key,
    typename Hash = std::hash<Key>,
    typename KeyEqual = std::equal_to<Key>>
using NodeSet = std::unordered_set<Key, Hash, KeyEqual>;

#endif // MOXYGEN_USE_FOLLY

} // namespace moxygen::compat
