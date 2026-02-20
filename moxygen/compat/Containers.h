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
#else
#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <absl/container/node_hash_map.h>
#include <absl/container/node_hash_set.h>
#endif

#include <functional>

namespace moxygen::compat {

#if MOXYGEN_USE_FOLLY

// Folly mode: use F14 containers (SIMD-accelerated)
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

#else

// Std mode: use Abseil Swiss table containers (SIMD-accelerated)
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

#endif

} // namespace moxygen::compat
