/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <moxygen/MoQVersions.h>

#include <cstring>
#include <optional>

namespace moxygen {

/**
 * Protocol types supported by the picoquic MOQT server.
 */
enum class PicoProtocolType {
  Quic,          // MOQT over QUIC transport (ALPN: moqt-16, moqt-18)
  WebTransportH3 // MOQT over WebTransport over HTTP/3 (ALPN: h3)
};

/**
 * PicoProtocolDispatcher - Determines protocol type from ALPN.
 *
 * Used to route incoming connections to the appropriate handler:
 * - QUIC transport connections use PicoQuicWebTransport
 * - WebTransport connections use h3zero's HTTP/3 + WebTransport stack
 */
class PicoProtocolDispatcher {
 public:
  /**
   * Determine protocol type from negotiated ALPN string.
   * alpnSelectCallback guarantees only known ALPNs are negotiated, so
   * an unrecognized value here indicates a bug.
   */
  static std::optional<PicoProtocolType> getProtocol(const char* alpn) {
    if (!alpn) {
      return std::nullopt;
    }
    if (isH3(alpn)) {
      return PicoProtocolType::WebTransportH3;
    }
    if (isQuic(alpn)) {
      return PicoProtocolType::Quic;
    }
    return std::nullopt;
  }

  /**
   * Check if ALPN indicates HTTP/3 (for WebTransport).
   */
  static bool isH3(const char* alpn) {
    return alpn && strcmp(alpn, "h3") == 0;
  }

  /**
   * Check if ALPN names a supported MoQT version over QUIC (not HTTP/3).
   */
  static bool isQuic(const char* alpn) {
    return alpn && getVersionFromAlpn(alpn).has_value();
  }

  /**
   * Get human-readable name for protocol type.
   */
  static const char* protocolName(PicoProtocolType type) {
    switch (type) {
      case PicoProtocolType::Quic:
        return "Quic";
      case PicoProtocolType::WebTransportH3:
        return "WebTransportH3";
    }
    __builtin_unreachable();
  }
};

} // namespace moxygen
