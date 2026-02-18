/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <moxygen/compat/Config.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>

#if MOXYGEN_USE_FOLLY
#include <folly/Uri.h>

namespace moxygen::compat {
using Url = folly::Uri;

/**
 * Check if a string looks like a URL (has scheme://)
 * Standalone function for use with folly::Uri
 */
inline bool isUrl(std::string_view str) {
  auto colonPos = str.find("://");
  if (colonPos == std::string_view::npos || colonPos == 0) {
    return false;
  }
  // Check that scheme contains only valid characters
  for (size_t i = 0; i < colonPos; ++i) {
    char c = str[i];
    if (!std::isalnum(static_cast<unsigned char>(c)) &&
        c != '+' && c != '-' && c != '.') {
      return false;
    }
  }
  return true;
}

/**
 * MoQUrl - MoQ-specific URL utilities (folly::Uri version)
 *
 * Handles MoQ URL schemes:
 * - moq://host:port        - Raw QUIC MoQ
 * - moq-wt://host:port/path - WebTransport MoQ
 * - https://host:port/path  - WebTransport (standard URL)
 */
class MoQUrl {
 public:
  enum class TransportType {
    QUIC,           // Raw QUIC (moq:// or no path)
    WEBTRANSPORT    // WebTransport (moq-wt:// or https://)
  };

  explicit MoQUrl(const std::string& urlStr) : url_(urlStr), valid_(true) {
    determineTransportType();
  }

  explicit MoQUrl(const Url& url) : url_(url), valid_(true) {
    determineTransportType();
  }

  // Get parsed URL
  const Url& url() const { return url_; }

  // Get transport type
  TransportType transportType() const { return transportType_; }

  // Convenience accessors
  std::string host() const { return url_.host(); }
  uint16_t port() const { return url_.port(); }
  std::string path() const { return url_.path(); }

  // Is this a WebTransport URL?
  bool isWebTransport() const {
    return transportType_ == TransportType::WEBTRANSPORT;
  }

  // Is this a raw QUIC URL?
  bool isQuic() const {
    return transportType_ == TransportType::QUIC;
  }

  bool valid() const { return valid_; }

 private:
  void determineTransportType() {
    std::string scheme = url_.scheme();
    std::transform(scheme.begin(), scheme.end(), scheme.begin(), ::tolower);

    if (scheme == "moq-wt" || scheme == "https" || scheme == "wss") {
      transportType_ = TransportType::WEBTRANSPORT;
    } else if (scheme == "moq" || scheme == "quic") {
      transportType_ = TransportType::QUIC;
    } else {
      // Default to WebTransport for unknown schemes with paths
      std::string urlPath = url_.path();
      if (urlPath != "/" && !urlPath.empty()) {
        transportType_ = TransportType::WEBTRANSPORT;
      } else {
        transportType_ = TransportType::QUIC;
      }
    }
  }

  Url url_;
  TransportType transportType_{TransportType::QUIC};
  bool valid_{false};
};

} // namespace moxygen::compat

#else

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <regex>
#include <stdexcept>
#include <string>
#include <string_view>

namespace moxygen::compat {

/**
 * Url - Simple URL parser for std-mode
 *
 * Parses URLs in the format:
 *   scheme://[user:pass@]host[:port][/path][?query][#fragment]
 *
 * Supports:
 * - IPv4 addresses (127.0.0.1)
 * - IPv6 addresses with brackets ([::1])
 * - Hostnames (example.com)
 * - Default ports per scheme (http=80, https=443, moq=4433)
 *
 * Example:
 *   Url url("https://example.com:8080/path?query=1#frag");
 *   url.scheme();  // "https"
 *   url.host();    // "example.com"
 *   url.port();    // 8080
 *   url.path();    // "/path"
 *   url.query();   // "query=1"
 *   url.fragment();// "frag"
 */
class Url {
 public:
  Url() = default;

  /**
   * Parse a URL string
   * @throws std::invalid_argument if URL is malformed
   */
  explicit Url(std::string_view urlStr) {
    parse(std::string(urlStr));
  }

  explicit Url(const std::string& urlStr) {
    parse(urlStr);
  }

  explicit Url(const char* urlStr) {
    parse(std::string(urlStr));
  }

  // Accessors
  const std::string& scheme() const { return scheme_; }
  const std::string& host() const { return host_; }
  uint16_t port() const { return port_; }
  const std::string& path() const { return path_; }
  const std::string& query() const { return query_; }
  const std::string& fragment() const { return fragment_; }
  const std::string& userInfo() const { return userInfo_; }

  // Convenience accessors
  std::string authority() const {
    std::string result;
    if (!userInfo_.empty()) {
      result = userInfo_ + "@";
    }
    result += hostPort();
    return result;
  }

  std::string hostPort() const {
    if (port_ == defaultPortForScheme(scheme_)) {
      return host_;
    }
    if (isIPv6_) {
      return "[" + host_ + "]:" + std::to_string(port_);
    }
    return host_ + ":" + std::to_string(port_);
  }

  // Check if this is an IPv6 address
  bool isIPv6() const { return isIPv6_; }

  // Check if URL was successfully parsed
  bool valid() const { return valid_; }
  explicit operator bool() const { return valid_; }

  // Reconstruct full URL
  std::string toString() const {
    if (!valid_) return "";

    std::string result = scheme_ + "://";
    if (!userInfo_.empty()) {
      result += userInfo_ + "@";
    }
    if (isIPv6_) {
      result += "[" + host_ + "]";
    } else {
      result += host_;
    }
    if (port_ != defaultPortForScheme(scheme_)) {
      result += ":" + std::to_string(port_);
    }
    result += path_;
    if (!query_.empty()) {
      result += "?" + query_;
    }
    if (!fragment_.empty()) {
      result += "#" + fragment_;
    }
    return result;
  }

  // Static helpers

  /**
   * Get default port for a scheme
   */
  static uint16_t defaultPortForScheme(const std::string& scheme) {
    // Case-insensitive comparison
    std::string lowerScheme = scheme;
    std::transform(lowerScheme.begin(), lowerScheme.end(),
                   lowerScheme.begin(), ::tolower);

    if (lowerScheme == "http") return 80;
    if (lowerScheme == "https") return 443;
    if (lowerScheme == "ws") return 80;
    if (lowerScheme == "wss") return 443;
    if (lowerScheme == "moq") return 4433;
    if (lowerScheme == "moq-wt") return 443;
    if (lowerScheme == "quic") return 443;
    return 0;
  }

  /**
   * Parse without throwing (returns empty Url on failure)
   */
  static Url tryParse(const std::string& urlStr) noexcept {
    try {
      return Url(urlStr);
    } catch (...) {
      return Url();
    }
  }

  /**
   * Check if a string looks like a URL (has scheme://)
   */
  static bool isUrl(std::string_view str) {
    auto colonPos = str.find("://");
    if (colonPos == std::string_view::npos || colonPos == 0) {
      return false;
    }
    // Check that scheme contains only valid characters
    for (size_t i = 0; i < colonPos; ++i) {
      char c = str[i];
      if (!std::isalnum(static_cast<unsigned char>(c)) &&
          c != '+' && c != '-' && c != '.') {
        return false;
      }
    }
    return true;
  }

 private:
  void parse(const std::string& urlStr) {
    if (urlStr.empty()) {
      throw std::invalid_argument("Empty URL");
    }

    // Regex pattern for URL parsing
    // Groups: 1=scheme, 2=authority (with userinfo), 3=path, 4=query, 5=fragment
    static const std::regex urlRegex(
        R"(^([a-zA-Z][a-zA-Z0-9+.-]*):\/\/)"  // scheme
        R"(([^/?#]*))"                         // authority
        R"(([^?#]*))"                          // path
        R"(?:\?([^#]*))?)"                     // query (optional)
        R"(?:#(.*))?$)",                       // fragment (optional)
        std::regex::ECMAScript);

    std::smatch match;
    if (!std::regex_match(urlStr, match, urlRegex)) {
      throw std::invalid_argument("Invalid URL: " + urlStr);
    }

    scheme_ = match[1].str();
    std::string authority = match[2].str();
    path_ = match[3].str();
    if (match[4].matched) {
      query_ = match[4].str();
    }
    if (match[5].matched) {
      fragment_ = match[5].str();
    }

    // Parse authority: [userinfo@]host[:port]
    parseAuthority(authority);

    // Set default path if empty
    if (path_.empty()) {
      path_ = "/";
    }

    valid_ = true;
  }

  void parseAuthority(const std::string& authority) {
    if (authority.empty()) {
      throw std::invalid_argument("Missing host in URL");
    }

    std::string hostPort = authority;

    // Extract userinfo if present
    auto atPos = authority.find('@');
    if (atPos != std::string::npos) {
      userInfo_ = authority.substr(0, atPos);
      hostPort = authority.substr(atPos + 1);
    }

    // Check for IPv6 address
    if (!hostPort.empty() && hostPort[0] == '[') {
      // IPv6: [address]:port or [address]
      auto closeBracket = hostPort.find(']');
      if (closeBracket == std::string::npos) {
        throw std::invalid_argument("Invalid IPv6 address: missing ]");
      }
      host_ = hostPort.substr(1, closeBracket - 1);
      isIPv6_ = true;

      // Check for port
      if (closeBracket + 1 < hostPort.size()) {
        if (hostPort[closeBracket + 1] != ':') {
          throw std::invalid_argument(
              "Invalid characters after IPv6 address");
        }
        std::string portStr = hostPort.substr(closeBracket + 2);
        if (!portStr.empty()) {
          port_ = parsePort(portStr);
        }
      }
    } else {
      // IPv4 or hostname
      auto colonPos = hostPort.rfind(':');
      if (colonPos != std::string::npos) {
        host_ = hostPort.substr(0, colonPos);
        std::string portStr = hostPort.substr(colonPos + 1);
        if (!portStr.empty()) {
          port_ = parsePort(portStr);
        }
      } else {
        host_ = hostPort;
      }
    }

    if (host_.empty()) {
      throw std::invalid_argument("Empty host in URL");
    }

    // Apply default port if not specified
    if (port_ == 0) {
      port_ = defaultPortForScheme(scheme_);
    }
  }

  uint16_t parsePort(const std::string& portStr) {
    try {
      int port = std::stoi(portStr);
      if (port < 0 || port > 65535) {
        throw std::invalid_argument("Port out of range: " + portStr);
      }
      return static_cast<uint16_t>(port);
    } catch (const std::exception&) {
      throw std::invalid_argument("Invalid port: " + portStr);
    }
  }

  std::string scheme_;
  std::string host_;
  uint16_t port_{0};
  std::string path_;
  std::string query_;
  std::string fragment_;
  std::string userInfo_;
  bool isIPv6_{false};
  bool valid_{false};
};

/**
 * MoQUrl - MoQ-specific URL utilities
 *
 * Handles MoQ URL schemes:
 * - moq://host:port        - Raw QUIC MoQ
 * - moq-wt://host:port/path - WebTransport MoQ
 * - https://host:port/path  - WebTransport (standard URL)
 */
class MoQUrl {
 public:
  enum class TransportType {
    QUIC,           // Raw QUIC (moq:// or no path)
    WEBTRANSPORT    // WebTransport (moq-wt:// or https://)
  };

  explicit MoQUrl(const std::string& urlStr) : url_(urlStr) {
    determineTransportType();
  }

  explicit MoQUrl(const Url& url) : url_(url) {
    determineTransportType();
  }

  // Get parsed URL
  const Url& url() const { return url_; }

  // Get transport type
  TransportType transportType() const { return transportType_; }

  // Convenience accessors
  const std::string& host() const { return url_.host(); }
  uint16_t port() const { return url_.port(); }
  const std::string& path() const { return url_.path(); }

  // Is this a WebTransport URL?
  bool isWebTransport() const {
    return transportType_ == TransportType::WEBTRANSPORT;
  }

  // Is this a raw QUIC URL?
  bool isQuic() const {
    return transportType_ == TransportType::QUIC;
  }

  bool valid() const { return url_.valid(); }

 private:
  void determineTransportType() {
    if (!url_.valid()) {
      return;
    }

    std::string scheme = url_.scheme();
    std::transform(scheme.begin(), scheme.end(), scheme.begin(), ::tolower);

    if (scheme == "moq-wt" || scheme == "https" || scheme == "wss") {
      transportType_ = TransportType::WEBTRANSPORT;
    } else if (scheme == "moq" || scheme == "quic") {
      transportType_ = TransportType::QUIC;
    } else {
      // Default to WebTransport for unknown schemes with paths
      if (url_.path() != "/" && !url_.path().empty()) {
        transportType_ = TransportType::WEBTRANSPORT;
      } else {
        transportType_ = TransportType::QUIC;
      }
    }
  }

  Url url_;
  TransportType transportType_{TransportType::QUIC};
};

} // namespace moxygen::compat

#endif // !MOXYGEN_USE_FOLLY
