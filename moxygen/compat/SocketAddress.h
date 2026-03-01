/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <moxygen/compat/Config.h>

#if MOXYGEN_USE_FOLLY
#include <folly/SocketAddress.h>
#else

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>

namespace folly {

/**
 * Std-mode replacement for folly::SocketAddress
 *
 * Features:
 * - IPv4 and IPv6 support
 * - Bracket notation for IPv6 ([::1]:port)
 * - Conversion to/from sockaddr structures
 * - DNS hostname storage (no async resolution, just storage)
 */
class SocketAddress {
 public:
  enum class Family {
    UNSPECIFIED = AF_UNSPEC,
    INET = AF_INET,
    INET6 = AF_INET6
  };

  SocketAddress() = default;

  SocketAddress(const std::string& host, uint16_t port)
      : port_(port), initialized_(true) {
    setHost(host);
  }

  // Construct from sockaddr
  explicit SocketAddress(const struct sockaddr* addr) {
    setFromSockaddr(addr);
  }

  // Construct from sockaddr_storage
  explicit SocketAddress(const struct sockaddr_storage& storage) {
    setFromSockaddr(reinterpret_cast<const struct sockaddr*>(&storage));
  }

  // Check if address is set
  bool isInitialized() const {
    return initialized_;
  }

  // Check if address is empty/unset
  bool empty() const {
    return !initialized_;
  }

  // Get address family
  Family family() const {
    return family_;
  }

  // Check if this is an IPv4 address
  bool isIPv4() const {
    return family_ == Family::INET;
  }

  // Check if this is an IPv6 address
  bool isIPv6() const {
    return family_ == Family::INET6;
  }

  // Get host string (without brackets for IPv6)
  std::string getAddressStr() const {
    return host_;
  }

  // Get host with brackets if IPv6
  std::string getHostStr() const {
    if (family_ == Family::INET6) {
      return "[" + host_ + "]";
    }
    return host_;
  }

  // Get port
  uint16_t getPort() const {
    return port_;
  }

  // Get full address string (host:port, with brackets for IPv6)
  std::string describe() const {
    if (!initialized_) {
      return "<uninitialized>";
    }
    if (family_ == Family::INET6) {
      return "[" + host_ + "]:" + std::to_string(port_);
    }
    return host_ + ":" + std::to_string(port_);
  }

  // Set from host and port
  void setFromHostPort(const std::string& host, uint16_t port) {
    setHost(host);
    port_ = port;
    initialized_ = true;
  }

  // Set from IP address string and port
  void setFromIpPort(const std::string& ip, uint16_t port) {
    setHost(ip);
    port_ = port;
    initialized_ = true;
  }

  // Set from sockaddr
  void setFromSockaddr(const struct sockaddr* addr) {
    if (!addr) {
      reset();
      return;
    }

    char ipStr[INET6_ADDRSTRLEN];

    if (addr->sa_family == AF_INET) {
      const auto* addr4 = reinterpret_cast<const struct sockaddr_in*>(addr);
      inet_ntop(AF_INET, &addr4->sin_addr, ipStr, sizeof(ipStr));
      host_ = ipStr;
      port_ = ntohs(addr4->sin_port);
      family_ = Family::INET;
      initialized_ = true;
    } else if (addr->sa_family == AF_INET6) {
      const auto* addr6 = reinterpret_cast<const struct sockaddr_in6*>(addr);
      inet_ntop(AF_INET6, &addr6->sin6_addr, ipStr, sizeof(ipStr));
      host_ = ipStr;
      port_ = ntohs(addr6->sin6_port);
      family_ = Family::INET6;
      initialized_ = true;
    } else {
      reset();
    }
  }

  // Convert to sockaddr_storage
  // Returns true on success
  bool getAddress(struct sockaddr_storage* storage) const {
    if (!initialized_) {
      return false;
    }

    std::memset(storage, 0, sizeof(*storage));

    if (family_ == Family::INET) {
      auto* addr4 = reinterpret_cast<struct sockaddr_in*>(storage);
      addr4->sin_family = AF_INET;
      addr4->sin_port = htons(port_);
      if (inet_pton(AF_INET, host_.c_str(), &addr4->sin_addr) != 1) {
        return false;
      }
      return true;
    } else if (family_ == Family::INET6) {
      auto* addr6 = reinterpret_cast<struct sockaddr_in6*>(storage);
      addr6->sin6_family = AF_INET6;
      addr6->sin6_port = htons(port_);
      if (inet_pton(AF_INET6, host_.c_str(), &addr6->sin6_addr) != 1) {
        return false;
      }
      return true;
    }

    return false;
  }

  // Get sockaddr length for this address family
  socklen_t getActualSize() const {
    if (family_ == Family::INET) {
      return sizeof(struct sockaddr_in);
    } else if (family_ == Family::INET6) {
      return sizeof(struct sockaddr_in6);
    }
    return 0;
  }

  // Reset to uninitialized state
  void reset() {
    host_.clear();
    port_ = 0;
    family_ = Family::UNSPECIFIED;
    initialized_ = false;
  }

  bool operator==(const SocketAddress& other) const {
    if (!initialized_ && !other.initialized_) {
      return true;
    }
    return initialized_ == other.initialized_ && host_ == other.host_ &&
        port_ == other.port_ && family_ == other.family_;
  }

  bool operator!=(const SocketAddress& other) const {
    return !(*this == other);
  }

  // Static factory methods

  // Parse address with port (supports [ipv6]:port and ipv4:port)
  static std::optional<SocketAddress> tryParse(const std::string& hostPort) {
    if (hostPort.empty()) {
      return std::nullopt;
    }

    std::string host;
    uint16_t port = 0;

    // Check for IPv6 bracket notation
    if (hostPort[0] == '[') {
      auto closeBracket = hostPort.find(']');
      if (closeBracket == std::string::npos) {
        return std::nullopt;
      }
      host = hostPort.substr(1, closeBracket - 1);

      // Look for port after bracket
      if (closeBracket + 1 < hostPort.size()) {
        if (hostPort[closeBracket + 1] != ':') {
          return std::nullopt;
        }
        auto portStr = hostPort.substr(closeBracket + 2);
        try {
          int p = std::stoi(portStr);
          if (p < 0 || p > 65535) {
            return std::nullopt;
          }
          port = static_cast<uint16_t>(p);
        } catch (...) {
          return std::nullopt;
        }
      }
    } else {
      // IPv4 or hostname
      auto lastColon = hostPort.rfind(':');
      if (lastColon != std::string::npos) {
        host = hostPort.substr(0, lastColon);
        auto portStr = hostPort.substr(lastColon + 1);
        try {
          int p = std::stoi(portStr);
          if (p < 0 || p > 65535) {
            return std::nullopt;
          }
          port = static_cast<uint16_t>(p);
        } catch (...) {
          return std::nullopt;
        }
      } else {
        host = hostPort;
      }
    }

    if (host.empty()) {
      return std::nullopt;
    }

    return SocketAddress(host, port);
  }

 private:
  void setHost(const std::string& host) {
    // Strip brackets from IPv6 if present
    if (!host.empty() && host[0] == '[' && host.back() == ']') {
      host_ = host.substr(1, host.size() - 2);
    } else {
      host_ = host;
    }

    // Detect address family
    struct in_addr addr4;
    struct in6_addr addr6;

    if (inet_pton(AF_INET, host_.c_str(), &addr4) == 1) {
      family_ = Family::INET;
    } else if (inet_pton(AF_INET6, host_.c_str(), &addr6) == 1) {
      family_ = Family::INET6;
    } else {
      // Hostname - keep as unspecified (needs DNS resolution)
      family_ = Family::UNSPECIFIED;
    }
  }

  std::string host_;
  uint16_t port_{0};
  Family family_{Family::UNSPECIFIED};
  bool initialized_{false};
};

} // namespace folly

#endif // !MOXYGEN_USE_FOLLY

// Bring into compat namespace
namespace moxygen::compat {
using SocketAddress = folly::SocketAddress;

// Helper function to create SocketAddress from sockaddr*
// This works in both Folly and std mode
inline SocketAddress makeSocketAddress(const struct sockaddr* addr) {
  SocketAddress result;
  if (addr) {
#if MOXYGEN_USE_FOLLY
    result.setFromSockaddr(addr);
#else
    result.setFromSockaddr(addr);
#endif
  }
  return result;
}

// Parse host:port string (supports [ipv6]:port notation)
inline std::optional<SocketAddress> parseSocketAddress(
    const std::string& hostPort) {
#if MOXYGEN_USE_FOLLY
  try {
    return SocketAddress(hostPort, 0);  // Folly doesn't have tryParse
  } catch (...) {
    return std::nullopt;
  }
#else
  return SocketAddress::tryParse(hostPort);
#endif
}

} // namespace moxygen::compat
