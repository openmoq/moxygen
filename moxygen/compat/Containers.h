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
#endif

#include <functional>
#include <unordered_map>
#include <unordered_set>

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

#else // !MOXYGEN_USE_FOLLY

// Std mode: use standard containers
template <
    typename Key,
    typename Value,
    typename Hash = std::hash<Key>,
    typename KeyEqual = std::equal_to<Key>>
using FastMap = std::unordered_map<Key, Value, Hash, KeyEqual>;

template <
    typename Key,
    typename Hash = std::hash<Key>,
    typename KeyEqual = std::equal_to<Key>>
using FastSet = std::unordered_set<Key, Hash, KeyEqual>;

// NodeMap/NodeSet have stable references - std equivalents work similarly
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
