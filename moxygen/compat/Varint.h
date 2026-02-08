/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <moxygen/compat/Config.h>

#if MOXYGEN_USE_FOLLY && MOXYGEN_QUIC_MVFST
// Use mvfst's QUIC integer implementation when available
#include <quic/QuicException.h>
#include <quic/codec/QuicInteger.h>
#else
// Standalone QUIC varint implementation for std-mode or picoquic

#include <moxygen/compat/Expected.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>

namespace quic {

// Std-mode transport error codes (subset used by moxygen)
enum class TransportErrorCode : uint64_t {
  NO_ERROR = 0x0,
  INTERNAL_ERROR = 0x1,
  CONNECTION_REFUSED = 0x2,
  FLOW_CONTROL_ERROR = 0x3,
  STREAM_LIMIT_ERROR = 0x4,
  STREAM_STATE_ERROR = 0x5,
  FINAL_SIZE_ERROR = 0x6,
  FRAME_ENCODING_ERROR = 0x7,
  TRANSPORT_PARAMETER_ERROR = 0x8,
  CONNECTION_ID_LIMIT_ERROR = 0x9,
  PROTOCOL_VIOLATION = 0xA,
  INVALID_TOKEN = 0xB,
  APPLICATION_ERROR = 0xC,
  CRYPTO_BUFFER_EXCEEDED = 0xD,
  KEY_UPDATE_ERROR = 0xE,
  AEAD_LIMIT_REACHED = 0xF,
  NO_VIABLE_PATH = 0x10,
};

// QUIC varint size calculation - returns Expected for compatibility with Folly
// Maximum QUIC varint value is 2^62 - 1
constexpr uint64_t kMaxQuicIntegerValue = (1ULL << 62) - 1;

inline moxygen::compat::Expected<size_t, TransportErrorCode>
getQuicIntegerSize(uint64_t value) {
  if (value <= 63) {
    return 1;
  } else if (value <= 16383) {
    return 2;
  } else if (value <= 1073741823) {
    return 4;
  } else if (value <= kMaxQuicIntegerValue) {
    return 8;
  }
  return moxygen::compat::makeUnexpected(TransportErrorCode::INTERNAL_ERROR);
}

// Decode a QUIC variable-length integer from a buffer
// Returns the decoded value and advances the cursor
inline std::optional<uint64_t> decodeQuicInteger(
    const uint8_t*& data,
    size_t& len) {
  if (len == 0) {
    return std::nullopt;
  }

  uint8_t firstByte = *data;
  uint8_t type = (firstByte >> 6) & 0x3;
  size_t intLen = 1u << type;

  if (len < intLen) {
    return std::nullopt;
  }

  uint64_t value = firstByte & 0x3F;
  for (size_t i = 1; i < intLen; ++i) {
    value = (value << 8) | data[i];
  }

  data += intLen;
  len -= intLen;
  return value;
}

// Encode a QUIC variable-length integer into a buffer
// Returns number of bytes written, or 0 on error
inline size_t encodeQuicInteger(uint64_t value, uint8_t* data, size_t len) {
  auto maybeSizeResult = getQuicIntegerSize(value);
  if (maybeSizeResult.hasError()) {
    return 0;
  }
  size_t intLen = *maybeSizeResult;
  if (len < intLen) {
    return 0;
  }

  uint8_t type;
  switch (intLen) {
    case 1:
      type = 0;
      break;
    case 2:
      type = 1;
      break;
    case 4:
      type = 2;
      break;
    case 8:
      type = 3;
      break;
    default:
      return 0;
  }

  // Encode in big-endian with type prefix
  for (size_t i = intLen; i > 0; --i) {
    data[i - 1] = static_cast<uint8_t>(value & 0xFF);
    value >>= 8;
  }
  data[0] |= (type << 6);

  return intLen;
}

// follyutils compatibility namespace
namespace follyutils {

// Forward declaration - ByteCursor is defined in ByteCursor.h
// This decodeQuicInteger works with any cursor type that has:
// - totalLength() returning size_t
// - peek() returning uint8_t (to check first byte without advancing)
// - read<uint8_t>() to read and advance cursor
template <typename CursorType>
inline std::optional<std::pair<uint64_t, size_t>> decodeQuicInteger(
    CursorType& cursor) {
  size_t avail = cursor.totalLength();
  if (avail == 0) {
    return std::nullopt;
  }

  // Peek at first byte to determine varint length
  uint8_t firstByte = cursor.peek();
  uint8_t type = (firstByte >> 6) & 0x3;
  size_t intLen = 1u << type;

  if (avail < intLen) {
    return std::nullopt;
  }

  // Read bytes one at a time to handle cross-buffer varints correctly
  // First byte is read and masked
  uint64_t value = cursor.template read<uint8_t>() & 0x3F;
  for (size_t i = 1; i < intLen; ++i) {
    value = (value << 8) | cursor.template read<uint8_t>();
  }

  return std::make_pair(value, intLen);
}

// Overload that also checks and updates a length parameter
// This is used when parsing within a frame where length is the remaining
// bytes in the frame
template <typename CursorType>
inline std::optional<std::pair<uint64_t, size_t>> decodeQuicInteger(
    CursorType& cursor,
    size_t& length) {
  if (length == 0) {
    return std::nullopt;
  }

  size_t avail = std::min(cursor.totalLength(), length);
  if (avail == 0) {
    return std::nullopt;
  }

  // Peek at first byte to determine varint length
  uint8_t firstByte = cursor.peek();
  uint8_t type = (firstByte >> 6) & 0x3;
  size_t intLen = 1u << type;

  if (avail < intLen) {
    return std::nullopt;
  }

  // Read bytes one at a time to handle cross-buffer varints correctly
  // First byte is read and masked
  uint64_t value = cursor.template read<uint8_t>() & 0x3F;
  for (size_t i = 1; i < intLen; ++i) {
    value = (value << 8) | cursor.template read<uint8_t>();
  }

  // Note: We do NOT modify length here - the caller is expected to do:
  //   length -= result->second;
  // This matches Folly's behavior where decodeQuicInteger returns the
  // byte count but does not modify the length parameter.
  return std::make_pair(value, intLen);
}

} // namespace follyutils

} // namespace quic

#endif // !(MOXYGEN_USE_FOLLY && MOXYGEN_QUIC_MVFST)
