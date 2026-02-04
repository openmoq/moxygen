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
#include <netinet/in.h>
#include <sys/socket.h>

#include <cstdint>
#include <cstring>
#include <string>

namespace folly {

// Minimal std-mode replacement for folly::SocketAddress
class SocketAddress {
 public:
  SocketAddress() = default;

  SocketAddress(const std::string& host, uint16_t port)
      : host_(host), port_(port), initialized_(true) {}

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

  // Get host string
  std::string getAddressStr() const {
    return host_;
  }

  // Get port
  uint16_t getPort() const {
    return port_;
  }

  // Get full address string (host:port)
  std::string describe() const {
    if (!initialized_) {
      return "<uninitialized>";
    }
    return host_ + ":" + std::to_string(port_);
  }

  // Set from host and port
  void setFromHostPort(const std::string& host, uint16_t port) {
    host_ = host;
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
      initialized_ = true;
    } else if (addr->sa_family == AF_INET6) {
      const auto* addr6 = reinterpret_cast<const struct sockaddr_in6*>(addr);
      inet_ntop(AF_INET6, &addr6->sin6_addr, ipStr, sizeof(ipStr));
      host_ = ipStr;
      port_ = ntohs(addr6->sin6_port);
      initialized_ = true;
    } else {
      reset();
    }
  }

  // Reset to uninitialized state
  void reset() {
    host_.clear();
    port_ = 0;
    initialized_ = false;
  }

  bool operator==(const SocketAddress& other) const {
    if (!initialized_ && !other.initialized_) {
      return true;
    }
    return initialized_ == other.initialized_ && host_ == other.host_ &&
        port_ == other.port_;
  }

  bool operator!=(const SocketAddress& other) const {
    return !(*this == other);
  }

 private:
  std::string host_;
  uint16_t port_{0};
  bool initialized_{false};
};

} // namespace folly

#endif // !MOXYGEN_USE_FOLLY

// Bring into compat namespace
namespace moxygen::compat {
using SocketAddress = folly::SocketAddress;
} // namespace moxygen::compat
