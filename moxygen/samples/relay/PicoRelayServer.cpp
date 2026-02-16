/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// Picoquic-based MoQ Relay Server
//
// A relay server that uses PicoquicMoQServer for the QUIC transport
// and MoQRelayCompat for relay functionality.
//
// Usage:
//   ./picorelayserver --port 4443 --cert cert.pem --key key.pem

#include <moxygen/MoQSession.h>
#include <moxygen/MoQVersions.h>
#include <moxygen/compat/Callbacks.h>
#include <moxygen/compat/Debug.h>
#include <moxygen/compat/MoQServerInterface.h>
#include <moxygen/compat/SocketAddress.h>
#include <moxygen/events/MoQSimpleExecutor.h>
#include <moxygen/relay/MoQRelayCompat.h>
#include <moxygen/transports/PicoquicMoQServer.h>
#include <moxygen/transports/TransportMode.h>

#include <unistd.h>

#include <atomic>
#include <csignal>
#include <iostream>
#include <memory>

namespace {

using namespace moxygen;

// --- Command line arguments ---

struct Args {
  uint16_t port{4443};
  std::string certFile;
  std::string keyFile;
  std::string ns;  // allowed namespace prefix (optional)
  std::string transport{"quic"};  // quic, webtransport
};

Args parseArgs(int argc, char* argv[]) {
  Args args;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if ((arg == "-p" || arg == "--port") && i + 1 < argc) {
      args.port = static_cast<uint16_t>(std::stoi(argv[++i]));
    } else if ((arg == "-c" || arg == "--cert") && i + 1 < argc) {
      args.certFile = argv[++i];
    } else if ((arg == "-k" || arg == "--key") && i + 1 < argc) {
      args.keyFile = argv[++i];
    } else if ((arg == "-n" || arg == "--ns") && i + 1 < argc) {
      args.ns = argv[++i];
    } else if ((arg == "-t" || arg == "--transport") && i + 1 < argc) {
      args.transport = argv[++i];
    } else if (arg == "-h" || arg == "--help") {
      std::cout << "Usage: " << argv[0] << " [options]\n"
                << "Options:\n"
                << "  -p, --port <port>        Port to listen on (default: 4443)\n"
                << "  -c, --cert <file>        TLS certificate file\n"
                << "  -k, --key <file>         TLS private key file\n"
                << "  -n, --ns <prefix>        Allowed namespace prefix (optional)\n"
                << "  -t, --transport <mode>   Transport mode: quic, webtransport (default: quic)\n"
                << "  -h, --help               Show this help\n";
      exit(0);
    }
  }

  if (args.certFile.empty() || args.keyFile.empty()) {
    std::cerr << "Error: --cert and --key are required\n";
    exit(1);
  }

  return args;
}

// Global shutdown flag
volatile sig_atomic_t g_running = 1;

void signalHandler(int /*sig*/) {
  // Note: only use async-signal-safe operations here
  const char msg[] = "\nSignal received!\n";
  write(STDOUT_FILENO, msg, sizeof(msg) - 1);
  g_running = 0;
}

// Session handler that sets up the relay for each new connection
class RelaySessionHandler : public compat::MoQServerInterface::SessionHandler {
 public:
  explicit RelaySessionHandler(std::shared_ptr<MoQRelayCompat> relay)
      : relay_(std::move(relay)) {}

  compat::Expected<ServerSetup, SessionCloseErrorCode> onNewSession(
      std::shared_ptr<MoQSession> session,
      const ClientSetup& clientSetup) override {
    XLOG(INFO) << "New relay session";

    // Both publish and subscribe are handled by the relay
    session->setPublishHandler(relay_);
    session->setSubscribeHandler(relay_);

    // Negotiate version
    uint64_t selectedVersion = kVersionDraftCurrent;
    for (auto v : clientSetup.supportedVersions) {
      if (isSupportedVersion(v)) {
        selectedVersion = v;
        break;
      }
    }

    ServerSetup setup;
    setup.selectedVersion = selectedVersion;
    return setup;
  }

  void onSessionTerminated(std::shared_ptr<MoQSession> /*session*/) override {
    XLOG(INFO) << "Relay session terminated";
  }

 private:
  std::shared_ptr<MoQRelayCompat> relay_;
};

}  // namespace

int main(int argc, char* argv[]) {
  auto args = parseArgs(argc, argv);

  auto transportMode = moxygen::transports::transportModeFromString(args.transport);
  std::cout << "Starting picoquic relay server on port " << args.port
            << " transport=" << moxygen::transports::transportModeToString(transportMode)
            << "\n";

  // Create executor
  auto executor = std::make_shared<moxygen::MoQSimpleExecutor>();

  // Create the relay
  auto relay = std::make_shared<MoQRelayCompat>();
  if (!args.ns.empty()) {
    relay->setAllowedNamespacePrefix(TrackNamespace({args.ns}));
    std::cout << "Allowing namespace prefix: " << args.ns << "\n";
  }

  // Create session handler
  auto sessionHandler = std::make_shared<RelaySessionHandler>(relay);

  // Configure server
  moxygen::transports::PicoquicMoQServer::Config serverConfig;
  serverConfig.certFile = args.certFile;
  serverConfig.keyFile = args.keyFile;
  serverConfig.transportMode = transportMode;

  // Create server
  auto server = std::make_unique<moxygen::transports::PicoquicMoQServer>(
      executor, std::move(serverConfig));

  // Set up signal handling
  std::signal(SIGINT, signalHandler);
  std::signal(SIGTERM, signalHandler);

  // Start server
  moxygen::compat::SocketAddress addr("::", args.port);
  server->start(addr, sessionHandler);

  std::cout << "Relay server running on port " << args.port
            << ". Press Ctrl+C to stop.\n";

  // Run until shutdown
  while (g_running) {
    executor->runFor(std::chrono::milliseconds(100));
  }

  std::cout << "Shutting down relay server...\n";
  server->stop();

  return 0;
}
