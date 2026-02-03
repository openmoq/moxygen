/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <moxygen/compat/Config.h>

#if MOXYGEN_USE_FOLLY
#include <folly/hash/Hash.h>
#endif

#include <cstddef>
#include <functional>
#include <type_traits>

namespace moxygen::compat {

#if MOXYGEN_USE_FOLLY

// Folly mode: use folly::hash utilities
using folly::hash::hash_combine;
using folly::hash::hash_range;

// For to_underlying, use std::to_underlying (C++23) or folly's version
template <typename E>
constexpr auto to_underlying(E e) noexcept {
  return folly::to_underlying(e);
}

#else // !MOXYGEN_USE_FOLLY

// Std mode: provide compatible implementations

// Combine two hash values (boost/folly style)
inline size_t hash_combine(size_t seed, size_t value) noexcept {
  // Golden ratio constant for better distribution
  return seed ^ (value + 0x9e3779b9 + (seed << 6) + (seed >> 2));
}

// Variadic hash_combine
template <typename T, typename... Args>
inline size_t hash_combine(size_t seed, const T& value, const Args&... args) {
  seed = hash_combine(seed, std::hash<T>{}(value));
  if constexpr (sizeof...(args) > 0) {
    return hash_combine(seed, args...);
  }
  return seed;
}

// Hash a range of values
template <typename Iterator>
inline size_t hash_range(Iterator begin, Iterator end) noexcept {
  size_t seed = 0;
  for (; begin != end; ++begin) {
    seed = hash_combine(
        seed,
        std::hash<typename std::iterator_traits<Iterator>::value_type>{}(
            *begin));
  }
  return seed;
}

// Hash a range with a custom hasher
template <typename Iterator, typename Hash>
inline size_t hash_range(Iterator begin, Iterator end, Hash hasher) noexcept {
  size_t seed = 0;
  for (; begin != end; ++begin) {
    seed = hash_combine(seed, hasher(*begin));
  }
  return seed;
}

// to_underlying for enums (C++23 std::to_underlying equivalent)
template <typename E>
constexpr auto to_underlying(E e) noexcept {
  static_assert(std::is_enum_v<E>, "to_underlying requires an enum type");
  return static_cast<std::underlying_type_t<E>>(e);
}

#endif // MOXYGEN_USE_FOLLY

// Hash combiner helper class (works in both modes)
struct HashCombiner {
  size_t seed{0};

  template <typename T>
  void operator()(const T& value) {
    seed = hash_combine(seed, std::hash<T>{}(value));
  }

  size_t result() const noexcept {
    return seed;
  }
};

} // namespace moxygen::compat
