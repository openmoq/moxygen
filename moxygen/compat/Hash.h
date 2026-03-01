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
#include <tuple>
#include <type_traits>
#include <utility>

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

// =============================================================================
// Enum hash (generic, no specialization required)
// =============================================================================

struct EnumHash {
  template <typename E>
  std::enable_if_t<std::is_enum_v<E>, size_t>
  operator()(E e) const noexcept {
    return mixHash(static_cast<uint64_t>(to_underlying(e)));
  }
};

// =============================================================================
// Pair hash
// =============================================================================

struct PairHash {
  template <typename T1, typename T2>
  size_t operator()(const std::pair<T1, T2>& p) const noexcept {
    size_t h1 = std::hash<T1>{}(p.first);
    size_t h2 = std::hash<T2>{}(p.second);
    return hash_combine(h1, h2);
  }
};

// Pair hash with custom hashers
template <typename Hash1 = void, typename Hash2 = void>
struct PairHasher {
  template <typename T1, typename T2>
  size_t operator()(const std::pair<T1, T2>& p) const noexcept {
    size_t h1, h2;
    if constexpr (std::is_void_v<Hash1>) {
      h1 = std::hash<T1>{}(p.first);
    } else {
      h1 = Hash1{}(p.first);
    }
    if constexpr (std::is_void_v<Hash2>) {
      h2 = std::hash<T2>{}(p.second);
    } else {
      h2 = Hash2{}(p.second);
    }
    return hash_combine(h1, h2);
  }
};

// =============================================================================
// Tuple hash
// =============================================================================

namespace detail {

template <typename Tuple, size_t... Is>
size_t hashTupleImpl(const Tuple& t, std::index_sequence<Is...>) noexcept {
  size_t seed = 0;
  ((seed = hash_combine(
        seed,
        std::hash<std::tuple_element_t<Is, Tuple>>{}(std::get<Is>(t)))),
   ...);
  return seed;
}

} // namespace detail

struct TupleHash {
  template <typename... Args>
  size_t operator()(const std::tuple<Args...>& t) const noexcept {
    return detail::hashTupleImpl(t, std::index_sequence_for<Args...>{});
  }
};

// =============================================================================
// Transparent string hash (for heterogeneous lookup)
// =============================================================================
// Allows lookup in unordered_map<string, V> using string_view without
// constructing a temporary string.

struct TransparentStringHash {
  using is_transparent = void;  // Enable heterogeneous lookup

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

struct TransparentStringEqual {
  using is_transparent = void;

  bool operator()(std::string_view a, std::string_view b) const noexcept {
    return a == b;
  }
};

// =============================================================================
// FNV-1a hash (fast, non-cryptographic)
// =============================================================================
// Good for general-purpose hashing when security isn't a concern.
// Faster than SipHash for small inputs.

namespace detail {

inline constexpr uint64_t kFnvOffset64 = 0xcbf29ce484222325ULL;
inline constexpr uint64_t kFnvPrime64 = 0x100000001b3ULL;

inline constexpr size_t fnv1a(const uint8_t* data, size_t len) noexcept {
  uint64_t hash = kFnvOffset64;
  for (size_t i = 0; i < len; ++i) {
    hash ^= static_cast<uint64_t>(data[i]);
    hash *= kFnvPrime64;
  }
  return static_cast<size_t>(hash);
}

} // namespace detail

struct FnvHash {
  size_t operator()(std::string_view sv) const noexcept {
    return detail::fnv1a(
        reinterpret_cast<const uint8_t*>(sv.data()), sv.size());
  }

  size_t operator()(const std::string& s) const noexcept {
    return (*this)(std::string_view(s));
  }

  // Hash raw bytes
  size_t operator()(const void* data, size_t len) const noexcept {
    return detail::fnv1a(static_cast<const uint8_t*>(data), len);
  }
};

// =============================================================================
// Constexpr string hash (compile-time)
// =============================================================================
// Use for switch statements on strings, compile-time string comparison, etc.
//
// Example:
//   switch (constexprHash(str)) {
//     case "hello"_hash: ...
//     case "world"_hash: ...
//   }

constexpr size_t constexprHash(std::string_view sv) noexcept {
  uint64_t hash = detail::kFnvOffset64;
  for (char c : sv) {
    hash ^= static_cast<uint64_t>(static_cast<uint8_t>(c));
    hash *= detail::kFnvPrime64;
  }
  return static_cast<size_t>(hash);
}

// User-defined literal for compile-time string hashing
constexpr size_t operator""_hash(const char* str, size_t len) noexcept {
  return constexprHash(std::string_view(str, len));
}

// =============================================================================
// WyHash - extremely fast hash function
// =============================================================================
// Modern hash function with excellent speed and distribution.
// Based on wyhash by Wang Yi: https://github.com/wangyi-fudan/wyhash
// This is a simplified version for common use cases.

namespace detail {

inline uint64_t wyMix(uint64_t a, uint64_t b) noexcept {
  // Multiply and xor high/low parts
  __uint128_t r = static_cast<__uint128_t>(a) * b;
  return static_cast<uint64_t>(r) ^ static_cast<uint64_t>(r >> 64);
}

inline uint64_t wyRead8(const uint8_t* p) noexcept {
  uint64_t v;
  std::memcpy(&v, p, 8);
  return v;
}

inline uint64_t wyRead4(const uint8_t* p) noexcept {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return v;
}

inline constexpr uint64_t kWyP0 = 0xa0761d6478bd642fULL;
inline constexpr uint64_t kWyP1 = 0xe7037ed1a0b428dbULL;
inline constexpr uint64_t kWyP2 = 0x8ebc6af09c88c6e3ULL;
inline constexpr uint64_t kWyP3 = 0x589965cc75374cc3ULL;

inline uint64_t wyhash(const uint8_t* data, size_t len, uint64_t seed) noexcept {
  const uint8_t* p = data;
  uint64_t a, b;

  if (len <= 16) {
    if (len >= 4) {
      a = (wyRead4(p) << 32) | wyRead4(p + ((len >> 3) << 2));
      b = (wyRead4(p + len - 4) << 32) | wyRead4(p + len - 4 - ((len >> 3) << 2));
    } else if (len > 0) {
      a = (static_cast<uint64_t>(p[0]) << 16) |
          (static_cast<uint64_t>(p[len >> 1]) << 8) | p[len - 1];
      b = 0;
    } else {
      a = b = 0;
    }
  } else {
    size_t i = len;
    if (i > 48) {
      uint64_t s1 = seed, s2 = seed;
      do {
        seed = wyMix(wyRead8(p) ^ kWyP1, wyRead8(p + 8) ^ seed);
        s1 = wyMix(wyRead8(p + 16) ^ kWyP2, wyRead8(p + 24) ^ s1);
        s2 = wyMix(wyRead8(p + 32) ^ kWyP3, wyRead8(p + 40) ^ s2);
        p += 48;
        i -= 48;
      } while (i > 48);
      seed ^= s1 ^ s2;
    }
    while (i > 16) {
      seed = wyMix(wyRead8(p) ^ kWyP1, wyRead8(p + 8) ^ seed);
      p += 16;
      i -= 16;
    }
    a = wyRead8(p + i - 16);
    b = wyRead8(p + i - 8);
  }

  return wyMix(kWyP1 ^ len, wyMix(a ^ kWyP1, b ^ seed));
}

// Global wyhash seed
struct WyHashSeed {
  uint64_t seed;
  WyHashSeed() noexcept {
    std::random_device rd;
    seed = (static_cast<uint64_t>(rd()) << 32) | rd();
  }
};

inline uint64_t getWyHashSeed() noexcept {
  static const WyHashSeed s;
  return s.seed;
}

} // namespace detail

struct WyHash {
  size_t operator()(std::string_view sv) const noexcept {
    return static_cast<size_t>(detail::wyhash(
        reinterpret_cast<const uint8_t*>(sv.data()),
        sv.size(),
        detail::getWyHashSeed()));
  }

  size_t operator()(const std::string& s) const noexcept {
    return (*this)(std::string_view(s));
  }

  size_t operator()(const void* data, size_t len) const noexcept {
    return static_cast<size_t>(detail::wyhash(
        static_cast<const uint8_t*>(data), len, detail::getWyHashSeed()));
  }

  // Hash integers directly
  template <typename T>
  std::enable_if_t<std::is_integral_v<T>, size_t>
  operator()(T val) const noexcept {
    return static_cast<size_t>(
        detail::wyMix(static_cast<uint64_t>(val) ^ detail::kWyP0,
                      detail::getWyHashSeed() ^ detail::kWyP1));
  }
};

#endif // !MOXYGEN_USE_FOLLY

// =============================================================================
// Cross-mode utilities (available in both Folly and std modes)
// =============================================================================

// Hash bytes directly (useful for custom types)
inline size_t hashBytes(const void* data, size_t len) noexcept {
#if MOXYGEN_USE_FOLLY
  return folly::hash::SpookyHashV2::Hash64(data, len, 0);
#else
  const auto& key = detail::getSipHashKey();
  return static_cast<size_t>(detail::siphash24(
      static_cast<const uint8_t*>(data), len, key.k0, key.k1));
#endif
}

} // namespace moxygen::compat
