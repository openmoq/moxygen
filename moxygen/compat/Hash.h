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

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <random>
#include <string>
#include <string_view>
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

// =============================================================================
// SipHash-2-4 Implementation
// =============================================================================
// High-quality hash function resistant to hash flooding attacks.
// Used for string keys in hash maps to prevent DoS attacks.
//
// SipHash was created by Jean-Philippe Aumasson and Daniel J. Bernstein.
// Reference paper: "SipHash: a fast short-input PRF"
// Paper URL: https://131002.net/siphash/siphash.pdf
// Reference implementation: https://github.com/veorq/SipHash (CC0 1.0 Public Domain)
//
// This implementation follows the reference C implementation which is
// released into the public domain (CC0 1.0 Universal).
// =============================================================================

namespace detail {

// SipHash constants
inline constexpr uint64_t kSipRound0 = 0x736f6d6570736575ULL;
inline constexpr uint64_t kSipRound1 = 0x646f72616e646f6dULL;
inline constexpr uint64_t kSipRound2 = 0x6c7967656e657261ULL;
inline constexpr uint64_t kSipRound3 = 0x7465646279746573ULL;

inline uint64_t rotl64(uint64_t x, int b) noexcept {
  return (x << b) | (x >> (64 - b));
}

inline void sipround(uint64_t& v0, uint64_t& v1, uint64_t& v2, uint64_t& v3) noexcept {
  v0 += v1;
  v1 = rotl64(v1, 13);
  v1 ^= v0;
  v0 = rotl64(v0, 32);
  v2 += v3;
  v3 = rotl64(v3, 16);
  v3 ^= v2;
  v0 += v3;
  v3 = rotl64(v3, 21);
  v3 ^= v0;
  v2 += v1;
  v1 = rotl64(v1, 17);
  v1 ^= v2;
  v2 = rotl64(v2, 32);
}

// Read 8 bytes as little-endian uint64
inline uint64_t loadLE64(const uint8_t* p) noexcept {
  uint64_t result;
  std::memcpy(&result, p, 8);
  // Assume little-endian (x86/ARM)
  return result;
}

// SipHash-2-4 core
inline uint64_t siphash24(
    const uint8_t* data,
    size_t len,
    uint64_t k0,
    uint64_t k1) noexcept {
  uint64_t v0 = k0 ^ kSipRound0;
  uint64_t v1 = k1 ^ kSipRound1;
  uint64_t v2 = k0 ^ kSipRound2;
  uint64_t v3 = k1 ^ kSipRound3;

  const uint8_t* end = data + (len & ~7ULL);
  const uint8_t* p = data;

  // Process 8-byte blocks
  while (p < end) {
    uint64_t m = loadLE64(p);
    v3 ^= m;
    sipround(v0, v1, v2, v3);
    sipround(v0, v1, v2, v3);
    v0 ^= m;
    p += 8;
  }

  // Process remaining bytes
  uint64_t b = static_cast<uint64_t>(len) << 56;
  switch (len & 7) {
    case 7: b |= static_cast<uint64_t>(p[6]) << 48; [[fallthrough]];
    case 6: b |= static_cast<uint64_t>(p[5]) << 40; [[fallthrough]];
    case 5: b |= static_cast<uint64_t>(p[4]) << 32; [[fallthrough]];
    case 4: b |= static_cast<uint64_t>(p[3]) << 24; [[fallthrough]];
    case 3: b |= static_cast<uint64_t>(p[2]) << 16; [[fallthrough]];
    case 2: b |= static_cast<uint64_t>(p[1]) << 8;  [[fallthrough]];
    case 1: b |= static_cast<uint64_t>(p[0]);       break;
    case 0: break;
  }

  v3 ^= b;
  sipround(v0, v1, v2, v3);
  sipround(v0, v1, v2, v3);
  v0 ^= b;

  // Finalization
  v2 ^= 0xff;
  sipround(v0, v1, v2, v3);
  sipround(v0, v1, v2, v3);
  sipround(v0, v1, v2, v3);
  sipround(v0, v1, v2, v3);

  return v0 ^ v1 ^ v2 ^ v3;
}

// Global random key, initialized once at startup
struct SipHashKey {
  uint64_t k0;
  uint64_t k1;

  SipHashKey() noexcept {
    std::random_device rd;
    k0 = (static_cast<uint64_t>(rd()) << 32) | rd();
    k1 = (static_cast<uint64_t>(rd()) << 32) | rd();
  }
};

inline const SipHashKey& getSipHashKey() noexcept {
  static const SipHashKey key;
  return key;
}

} // namespace detail

// =============================================================================
// Secure hash for strings (hash flooding resistant)
// =============================================================================

struct SecureStringHash {
  size_t operator()(std::string_view sv) const noexcept {
    const auto& key = detail::getSipHashKey();
    return static_cast<size_t>(detail::siphash24(
        reinterpret_cast<const uint8_t*>(sv.data()),
        sv.size(),
        key.k0,
        key.k1));
  }

  size_t operator()(const std::string& s) const noexcept {
    return (*this)(std::string_view(s));
  }

  size_t operator()(const char* s) const noexcept {
    return (*this)(std::string_view(s));
  }
};

// =============================================================================
// Standard hash utilities
// =============================================================================

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

#if !MOXYGEN_USE_FOLLY

// =============================================================================
// Integer hash mixer (avalanche function)
// =============================================================================
// std::hash<int> is often identity, which causes clustering with sequential keys.
// This mixer provides good distribution for integer keys.
//
// Based on splitmix64/murmurhash finalizer

// Mix hash for any integer type up to 64 bits
template <typename T>
inline std::enable_if_t<std::is_integral_v<T>, size_t>
mixHash(T val) noexcept {
  uint64_t x = static_cast<uint64_t>(val);
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdULL;
  x ^= x >> 33;
  x *= 0xc4ceb9fe1a85ec53ULL;
  x ^= x >> 33;
  return static_cast<size_t>(x);
}

// Improved hash for integers
struct IntegerHash {
  template <typename T>
  std::enable_if_t<std::is_integral_v<T>, size_t>
  operator()(T x) const noexcept {
    return mixHash(x);
  }
};

// Improved hash for pointers
struct PointerHash {
  template <typename T>
  size_t operator()(T* ptr) const noexcept {
    return mixHash(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(ptr)));
  }
};

#endif // !MOXYGEN_USE_FOLLY

} // namespace moxygen::compat
