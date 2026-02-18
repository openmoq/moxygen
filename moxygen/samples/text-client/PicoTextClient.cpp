/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// Picoquic-based MoQ Text Client
//
// A simplified text client that uses PicoquicMoQClient for the QUIC transport.
// Subscribes to a track and prints received objects to stdout.
//
// This is the std-mode (no Folly) equivalent of MoQTextClient.cpp.

#include <moxygen/MoQConsumers.h>
#include <moxygen/MoQSession.h>
#include <moxygen/MoQTypes.h>
#include <moxygen/MoQVersions.h>
#include <moxygen/Publisher.h>
#include <moxygen/Subscriber.h>
#include <moxygen/compat/Callbacks.h>
#include <moxygen/compat/Debug.h>
#include <moxygen/compat/MoQClientInterface.h>
#include <moxygen/compat/SocketAddress.h>
#include <moxygen/events/MoQSimpleExecutor.h>
#include <moxygen/transports/PicoquicMoQClient.h>
#include <moxygen/transports/TransportMode.h>

#include <moxygen/compat/Config.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

using namespace moxygen;

// Helper to convert payload to string (works with both Folly and std-mode)
inline std::string payloadToString(Payload& payload) {
#if MOXYGEN_USE_FOLLY
  return payload->moveToFbString().toStdString();
#else
  return payload->moveToString();
#endif
}

// Helper to append payload bytes to a vector (handles IOBuf chains in Folly mode)
inline void appendPayloadBytes(
    Payload& payload,
    std::vector<uint8_t>& dest) {
#if MOXYGEN_USE_FOLLY
  // Coalesce IOBuf chain into a single buffer, then copy
  payload->coalesce();
#endif
  auto* data = payload->data();
  auto sz = payload->length();
  dest.insert(dest.end(), data, data + sz);
}

// --- Command line arguments ---

struct Args {
  std::string host;
  uint16_t port{4433};
  std::string ns{"moq-date"};
  std::string nsDelimiter{"/"};
  std::string track{"date"};
  std::string transport{"quic"}; // quic, webtransport
  std::string wtPath{"/moq"};    // WebTransport endpoint path
  bool insecure{false};
  int connectTimeout{5000};
};

Args parseArgs(int argc, char* argv[]) {
  Args args;
  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    if ((arg == "--host" || arg == "-H") && i + 1 < argc) {
      args.host = argv[++i];
    } else if ((arg == "--port" || arg == "-p") && i + 1 < argc) {
      args.port = static_cast<uint16_t>(std::stoi(argv[++i]));
    } else if ((arg == "--ns" || arg == "-n") && i + 1 < argc) {
      args.ns = argv[++i];
    } else if (arg == "--ns-delimiter" && i + 1 < argc) {
      args.nsDelimiter = argv[++i];
    } else if ((arg == "--track") && i + 1 < argc) {
      args.track = argv[++i];
    } else if ((arg == "--transport" || arg == "-T") && i + 1 < argc) {
      args.transport = argv[++i];
    } else if ((arg == "--path" || arg == "--endpoint") && i + 1 < argc) {
      args.wtPath = argv[++i];
    } else if (arg == "--insecure" || arg == "-k") {
      args.insecure = true;
    } else if (arg == "--timeout" && i + 1 < argc) {
      args.connectTimeout = std::stoi(argv[++i]);
    } else if (arg == "--help" || arg == "-h") {
      std::cout
          << "Usage: picotextclient [options]\n"
          << "  --host, -H <host>          Server hostname (required)\n"
          << "  --port, -p <port>          Server port (default: 4433)\n"
          << "  --ns,   -n <namespace>     Track namespace (default: moq-date)\n"
          << "  --ns-delimiter <delim>     Namespace delimiter (default: /)\n"
          << "  --track <name>             Track name (default: date)\n"
          << "  --transport, -T <type>     quic|webtransport (default: quic)\n"
          << "  --path, --endpoint <path>  WebTransport endpoint path (default: /moq)\n"
          << "                             Use /moq-relay for Mode 1 (Folly) relay\n"
          << "  --insecure, -k             Skip TLS certificate validation\n"
          << "  --timeout <ms>             Connect timeout in ms (default: 5000)\n"
          << "  --help, -h                 Show this help\n";
      std::exit(0);
    } else {
      std::cerr << "Unknown argument: " << arg << "\n";
      std::exit(1);
    }
  }
  if (args.host.empty()) {
    std::cerr << "Error: --host is required\n";
    std::exit(1);
  }
  return args;
}

// --- Simple SubgroupReceiver (std-mode, no Folly IOBufQueue) ---

class SimpleSubgroupReceiver : public SubgroupConsumer {
 public:
  explicit SimpleSubgroupReceiver(
      uint64_t groupId,
      uint64_t subgroupId)
      : groupId_(groupId), subgroupId_(subgroupId) {}

  compat::Expected<compat::Unit, MoQPublishError> object(
      uint64_t objectID,
      Payload payload,
      Extensions extensions,
      bool finSubgroup) override {
    if (payload) {
      std::string data = payloadToString(payload);
      std::cout << data;
      if (objectID > 0) {
        // After the minute prefix (object 0), seconds are separate objects
        std::cout << std::flush;
      }
    }
    if (finSubgroup) {
      std::cout << "\n" << std::flush;
    }
    return compat::unit;
  }

  compat::Expected<compat::Unit, MoQPublishError> beginObject(
      uint64_t objectID,
      uint64_t length,
      Payload initialPayload,
      Extensions extensions) override {
    pendingLength_ = length;
    pendingData_.clear();
    if (initialPayload) {
      appendPayloadBytes(initialPayload, pendingData_);
    }
    if (pendingData_.size() >= pendingLength_) {
      std::cout << std::string(pendingData_.begin(), pendingData_.end());
      std::cout << std::flush;
    }
    return compat::unit;
  }

  compat::Expected<ObjectPublishStatus, MoQPublishError> objectPayload(
      Payload payload,
      bool finSubgroup) override {
    if (payload) {
      appendPayloadBytes(payload, pendingData_);
    }
    if (pendingData_.size() >= pendingLength_) {
      std::cout << std::string(pendingData_.begin(), pendingData_.end());
      std::cout << std::flush;
      return ObjectPublishStatus::DONE;
    }
    return ObjectPublishStatus::IN_PROGRESS;
  }

  compat::Expected<compat::Unit, MoQPublishError> endOfGroup(
      uint64_t endOfGroupObjectID) override {
    std::cout << "\n[EndOfGroup g=" << groupId_
              << " obj=" << endOfGroupObjectID << "]\n"
              << std::flush;
    return compat::unit;
  }

  compat::Expected<compat::Unit, MoQPublishError> endOfTrackAndGroup(
      uint64_t endOfTrackObjectID) override {
    std::cout << "\n[EndOfTrackAndGroup g=" << groupId_
              << " obj=" << endOfTrackObjectID << "]\n"
              << std::flush;
    return compat::unit;
  }

  compat::Expected<compat::Unit, MoQPublishError> endOfSubgroup() override {
    return compat::unit;
  }

  void reset(ResetStreamErrorCode error) override {
    std::cerr << "[Stream reset error="
              << static_cast<uint32_t>(error) << "]\n";
  }

 private:
  uint64_t groupId_;
  uint64_t subgroupId_;
  uint64_t pendingLength_{0};
  std::vector<uint8_t> pendingData_;
};

// --- Simple TrackReceiver (std-mode, no Folly dependencies) ---

class SimpleTrackReceiver : public TrackConsumer {
 public:
  SimpleTrackReceiver() = default;

  compat::Expected<compat::Unit, MoQPublishError> setTrackAlias(
      TrackAlias alias) override {
    trackAlias_ = alias;
    return compat::unit;
  }

  compat::Expected<std::shared_ptr<SubgroupConsumer>, MoQPublishError>
  beginSubgroup(
      uint64_t groupID,
      uint64_t subgroupID,
      Priority priority,
      bool /*containsLastInGroup*/ = false) override {
    return std::make_shared<SimpleSubgroupReceiver>(groupID, subgroupID);
  }

  compat::Expected<compat::SemiFuture<compat::Unit>, MoQPublishError>
  awaitStreamCredit() override {
    return compat::makeSemiFuture();
  }

  compat::Expected<compat::Unit, MoQPublishError> objectStream(
      const ObjectHeader& header,
      Payload payload,
      bool /*lastInGroup*/ = false) override {
    if (header.status != ObjectStatus::NORMAL) {
      std::cout << "[ObjectStatus g=" << header.group
                << " obj=" << header.id
                << " status=" << static_cast<uint32_t>(header.status)
                << "]\n"
                << std::flush;
      return compat::unit;
    }

    if (payload) {
      std::cout << payloadToString(payload) << std::flush;
    }
    return compat::unit;
  }

  compat::Expected<compat::Unit, MoQPublishError> datagram(
      const ObjectHeader& header,
      Payload payload,
      bool /*lastInGroup*/ = false) override {
    if (payload) {
      std::cout << payloadToString(payload) << std::flush;
    }
    return compat::unit;
  }

  compat::Expected<compat::Unit, MoQPublishError> publishDone(
      PublishDone pubDone) override {
    std::cout << "\n[PublishDone requestID=" << pubDone.requestID << "]\n"
              << std::flush;
    done_ = true;
    return compat::unit;
  }

  bool isDone() const {
    return done_;
  }

 private:
  std::optional<TrackAlias> trackAlias_;
  std::atomic<bool> done_{false};
};

// --- Text Client Subscriber ---

class TextClientSubscriber : public Subscriber {
 public:
  void publishNamespaceWithCallback(
      PublishNamespace ann,
      std::shared_ptr<PublishNamespaceCallback> cancelCallback,
      std::shared_ptr<compat::ResultCallback<
          std::shared_ptr<PublishNamespaceHandle>,
          PublishNamespaceError>> callback) override {
    XLOG(INFO) << "PublishNamespace ns=" << ann.trackNamespace << "\n";
    // Accept any namespace announcement
    callback->onSuccess(std::make_shared<PublishNamespaceHandle>(
        PublishNamespaceOk{
            .requestID = ann.requestID,
            .requestSpecificParams = {}}));
  }

  PublishResult publish(
      PublishRequest pub,
      std::shared_ptr<SubscriptionHandle> /*handle*/) override {
    return compat::makeUnexpected(PublishError{
        pub.requestID,
        PublishErrorCode::NOT_SUPPORTED,
        "Text client does not accept PUBLISH"});
  }
};

// --- Signal handling ---

std::atomic<bool> g_running{true};
MoQSimpleExecutor* g_executor{nullptr};

void signalHandler(int /*sig*/) {
  g_running.store(false);
  if (g_executor) {
    g_executor->stop();
  }
}

} // namespace

int main(int argc, char* argv[]) {
  auto args = parseArgs(argc, argv);

  auto transportMode = moxygen::transports::transportModeFromString(args.transport);
  std::cout << "Connecting to " << args.host << ":" << args.port
            << " ns='" << args.ns << "' track='" << args.track << "'"
            << " transport=" << moxygen::transports::transportModeToString(transportMode)
            << (args.insecure ? " (insecure)" : "") << "\n";

  // Create executor
  auto executor = std::make_shared<moxygen::MoQSimpleExecutor>();
  g_executor = executor.get();

  // Create client config
  moxygen::transports::PicoquicMoQClient::Config clientConfig;
  clientConfig.serverHost = args.host;
  clientConfig.serverPort = args.port;
  clientConfig.transportMode = transportMode;
  clientConfig.wtPath = args.wtPath;  // WebTransport endpoint path
  // ALPN is auto-selected based on transport mode
  clientConfig.insecure = args.insecure;
  clientConfig.idleTimeout = std::chrono::milliseconds(30000);

  // Create client
  auto client = std::make_unique<moxygen::transports::PicoquicMoQClient>(
      executor, std::move(clientConfig));

  // Create subscriber handler
  auto subscriber = std::make_shared<TextClientSubscriber>();

  // Create track receiver
  auto trackReceiver = std::make_shared<SimpleTrackReceiver>();

  // Set up signal handling
  std::signal(SIGINT, signalHandler);
  std::signal(SIGTERM, signalHandler);

  // Track namespace and name
  auto trackNs =
      moxygen::TrackNamespace(args.ns, args.nsDelimiter);
  auto fullTrackName =
      moxygen::FullTrackName({trackNs, args.track});

  // Connection state
  std::shared_ptr<moxygen::MoQSession> session;
  std::shared_ptr<moxygen::Publisher::SubscriptionHandle> subscription;

  // Connect with callback
  auto connectCallback = std::make_shared<
      moxygen::compat::LambdaResultCallback<
          moxygen::compat::MoQClientInterface::ConnectResult,
          moxygen::SessionCloseErrorCode>>(
      [&](moxygen::compat::MoQClientInterface::ConnectResult result) {
        session = result.session;
        std::cout << "Connected! Server version="
                  << result.serverSetup.selectedVersion << "\n";

        // Now subscribe to the track
        auto subReq = moxygen::SubscribeRequest::make(
            fullTrackName,
            moxygen::kDefaultPriority,
            moxygen::GroupOrder::OldestFirst,
            /*forward=*/true,
            moxygen::LocationType::LargestObject);

        auto subscribeCb = std::make_shared<
            moxygen::compat::LambdaResultCallback<
                std::shared_ptr<moxygen::Publisher::SubscriptionHandle>,
                moxygen::SubscribeError>>(
            [&](std::shared_ptr<moxygen::Publisher::SubscriptionHandle>
                    handle) {
              subscription = handle;
              auto& subOk = handle->subscribeOk();
              std::cout << "Subscribed! requestID=" << subOk.requestID;
              if (subOk.largest) {
                std::cout << " largest={" << subOk.largest->group << ", "
                          << subOk.largest->object << "}";
              }
              std::cout << "\n";
            },
            [&](moxygen::SubscribeError err) {
              std::cerr << "Subscribe error: code="
                        << static_cast<uint32_t>(err.errorCode)
                        << " reason=" << err.reasonPhrase << "\n";
              g_running.store(false);
              executor->stop();
            });

        session->subscribeWithCallback(
            std::move(subReq), trackReceiver, subscribeCb);
      },
      [&](moxygen::SessionCloseErrorCode error) {
        std::cerr << "Connect failed: error="
                  << static_cast<uint32_t>(error) << "\n";
        g_running.store(false);
        executor->stop();
      });

  client->connectWithCallback(
      std::chrono::milliseconds(args.connectTimeout),
      /*publishHandler=*/nullptr,
      subscriber,
      connectCallback);

  // Run the executor event loop
  // This processes application-level tasks while the picoquic event loop
  // runs on its own thread.
  executor->run();

  // Cleanup
  if (subscription) {
    subscription->unsubscribe();
  }
  client->close(moxygen::SessionCloseErrorCode::NO_ERROR);

  std::cout << "Disconnected.\n";
  return 0;
}
