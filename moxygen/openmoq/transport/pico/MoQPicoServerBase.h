/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>
#include <moxygen/MoQServerBase.h>
#include <string>

// Forward declaration — avoids exposing picoquic.h to consumers
// (C struct typedefs must be at file scope, not inside a namespace)
typedef struct st_picoquic_quic_t picoquic_quic_t;
typedef struct st_picoquic_cnx_t picoquic_cnx_t;
typedef struct st_h3zero_callback_ctx_t h3zero_callback_ctx_t;
typedef struct st_picohttp_server_path_item_t picohttp_server_path_item_t;

namespace moxygen {

class MoQExecutor;
class PicoQuicWebTransport;

/**
 * WebTransport configuration for picoquic server.
 */
struct PicoWebTransportConfig {
  bool enableWebTransport{false};  // Enable HTTP/3 WebTransport support
  bool enableRawMoQ{true};         // Enable raw MoQ over QUIC (default)
  std::string wtEndpoint{"/moq"};  // WebTransport CONNECT endpoint path
  uint32_t wtMaxSessions{100};     // Max concurrent WebTransport sessions
};

/**
 * MoQPicoServerBase - shared picoquic machinery for MoQ servers.
 *
 * Holds the picoquic_quic_t context plus the picoCallback, ConnectionContext,
 * and onNewConnection logic shared by MoQPicoQuicServer (thread-based) and
 * MoQPicoQuicEventBaseServer (EventBase-based).
 *
 * Supports two connection modes:
 * - Raw MoQ: Direct MoQ over QUIC (ALPN: moqt-16, moqt-15, moq-00)
 * - WebTransport: MoQ over WebTransport over HTTP/3 (ALPN: h3)
 *
 * Subclasses must set executor_ before calling createQuicContext().
 */
class MoQPicoServerBase : public MoQServerBase {
 public:
  MoQPicoServerBase(std::string cert,
                    std::string key,
                    std::string endpoint,
                    std::string versions = "",
                    PicoWebTransportConfig wtConfig = {});
  ~MoQPicoServerBase() override;

  /**
   * Get WebTransport configuration.
   */
  const PicoWebTransportConfig& getWebTransportConfig() const {
    return wtConfig_;
  }

 protected:
  /**
   * Creates and configures the picoquic_quic_t context:
   *   picoquic_create + ALPN selection callback + cookie mode + BBR.
   * Sets quic_. executor_ must already be set.
   * Returns true on success, false on failure (logs error).
   */
  bool createQuicContext();

  /**
   * Frees quic_ and sets it to nullptr.
   */
  void destroyQuicContext();

  std::string cert_;
  std::string key_;
  std::string versions_;
  PicoWebTransportConfig wtConfig_;
  picoquic_quic_t* quic_{nullptr};
  // Shared ownership so it can be passed directly to createSession().
  // MoQPicoQuicServer stores PicoQuicExecutor here.
  // MoQPicoQuicEventBaseServer stores MoQFollyExecutorImpl with a no-op
  // deleter (caller retains ownership).
  std::shared_ptr<MoQExecutor> executor_;

  // h3zero context for WebTransport connections (nullptr if WT disabled)
  h3zero_callback_ctx_t* h3Ctx_{nullptr};

  /**
   * Called after a PicoQuicWebTransport is created for a new connection.
   * Override to configure the web transport (e.g. set wake timeout callback).
   * Default is a no-op.
   */
  virtual void onWebTransportCreated(
      PicoQuicWebTransport& /*wt*/) noexcept {}

 private:
  // Called from picoCallback via MoQPicoServerBaseCallbacks (friend).
  // Takes void* to keep picoquic_cnx_t out of this header.
  void onNewConnectionImpl(void* cnx);

  // Called for WebTransport connections (h3 ALPN)
  void onNewWebTransportConnectionImpl(void* cnx);

  // Initialize h3zero context for WebTransport support
  bool initH3Zero();

  // Cleanup h3zero context
  void destroyH3Zero();

  // WebTransport path table for h3zero
  std::unique_ptr<picohttp_server_path_item_t[]> wtPathTable_;

  friend struct MoQPicoServerBaseCallbacks;
};

} // namespace moxygen
