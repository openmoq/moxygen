/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <string>
#include <string_view>

namespace moxygen::transports {

/**
 * Transport mode for MoQ connections.
 *
 * QUIC: Direct MoQ over QUIC (raw QUIC streams)
 *   - Uses ALPN "moq-00" or "moqt-*" for version negotiation
 *   - Simpler, lower overhead
 *   - Requires both endpoints to support raw QUIC MoQ
 *
 * WEBTRANSPORT: MoQ over WebTransport over HTTP/3
 *   - Uses ALPN "h3" and HTTP CONNECT upgrade
 *   - Compatible with WebTransport clients (browsers)
 *   - Enables interop with mvfst/proxygen servers in WebTransport mode
 */
enum class TransportMode {
  QUIC,         // Raw QUIC with MoQ ALPN
  WEBTRANSPORT  // WebTransport over HTTP/3
};

// String conversion utilities
inline std::string_view transportModeToString(TransportMode mode) {
  switch (mode) {
    case TransportMode::QUIC:
      return "quic";
    case TransportMode::WEBTRANSPORT:
      return "webtransport";
  }
  return "unknown";
}

inline TransportMode transportModeFromString(std::string_view str) {
  if (str == "quic" || str == "QUIC") {
    return TransportMode::QUIC;
  }
  if (str == "webtransport" || str == "WEBTRANSPORT" || str == "wt" ||
      str == "h3") {
    return TransportMode::WEBTRANSPORT;
  }
  // Default to QUIC for backwards compatibility
  return TransportMode::QUIC;
}

} // namespace moxygen::transports
