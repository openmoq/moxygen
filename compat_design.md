# Moxygen Compat Layer Design

## Overview

The **compat layer** (`moxygen/compat/`) is an abstraction layer that enables moxygen to compile and run in two distinct modes:

1. **Folly Mode** (`MOXYGEN_USE_FOLLY=1`): Uses Meta's Folly library, proxygen, and mvfst for high-performance production deployments
2. **Std Mode** (`MOXYGEN_USE_FOLLY=0`): Pure C++20 implementation with no external dependencies beyond the QUIC stack

This design allows the same MoQ protocol implementation to be used across different deployment scenarios while maintaining performance parity.

---

## Build Configuration Matrix

| Configuration | `MOXYGEN_USE_FOLLY` | `MOXYGEN_QUIC_BACKEND` | Use Case |
|---------------|---------------------|------------------------|----------|
| Full Folly    | 1 | mvfst | Production (Meta infrastructure) |
| Folly+Picoquic| 1 | picoquic | Testing Folly code with picoquic |
| Std+Picoquic  | 0 | picoquic | Embedded/minimal deployments |
| Std+mvfst     | 0 | mvfst | Std-mode with pre-built mvfst |

Configuration is controlled via CMake options and compile-time macros in `moxygen/compat/Config.h`.

---

## Compat Layer Components

### Core Type Abstractions

| Compat Header | Folly Mode | Std Mode | Purpose |
|---------------|------------|----------|---------|
| `Config.h` | Defines `MOXYGEN_USE_FOLLY=1` | Defines `MOXYGEN_USE_FOLLY=0` | Build configuration |
| `Payload.h` | Wraps `folly::IOBuf` | `std::vector<uint8_t>` | Zero-copy buffer |
| `ByteBuffer.h` | N/A (uses IOBuf) | Custom buffer with SBO | Buffer with headroom/tailroom |
| `ByteBufferQueue.h` | N/A (uses IOBufQueue) | Deque of ByteBuffers | Buffer queue for streaming |
| `ByteCursor.h` | N/A (uses folly::Cursor) | Custom cursor | Read cursor for parsing |
| `BufferIO.h` | `folly::io::Cursor`, `IOBufQueue` | `ByteCursor`, `ByteBufferQueue` | Buffer I/O abstractions |
| `Expected.h` | `folly::Expected<T, E>` | Custom `Expected<T, E>` | Error handling |
| `Try.h` | `folly::Try<T>` | Custom `Try<T>` | Exception wrapper |
| `Async.h` | `folly::SemiFuture/Promise` | Custom `SemiFuture/Promise` | Async primitives |
| `Unit.h` | `folly::Unit` | Custom `Unit` | Void type for templates |

### Container Abstractions

| Compat Header | Folly Mode | Std Mode (Abseil) | Std Mode (Built-in) |
|---------------|------------|-------------------|---------------------|
| `Containers.h` | `folly::F14FastMap/Set` | `absl::flat_hash_map/set` | `RobinHoodMap/Set` |
| `Hash.h` | `folly::hash::hash_combine` | Custom `hash_combine` | SipHash, FNV-1a, WyHash |

### Logging and Debug

| Compat Header | Folly Mode | Std Mode | Purpose |
|---------------|------------|----------|---------|
| `Logging.h` | `folly/logging/xlog.h` | spdlog-based XLOG macros | Logging |
| `Debug.h` | `folly/logging/xlog.h` | CHECK/DCHECK macros | Assertions |

### Network Utilities

| Compat Header | Folly Mode | Std Mode | Purpose |
|---------------|------------|----------|---------|
| `SocketAddress.h` | `folly::SocketAddress` | Custom SocketAddress | Network address |
| `Url.h` | `folly::Uri` | Custom URL parser | URL parsing |
| `ConnectionId.h` | `quic::ConnectionId` | `std::array<uint8_t, 20>` | QUIC connection ID |

### Transport Abstractions

| Compat Header | Purpose |
|---------------|---------|
| `WebTransportInterface.h` | Abstract interface for WebTransport sessions |
| `MoQClientInterface.h` | Abstract interface for MoQ clients |
| `MoQServerInterface.h` | Abstract interface for MoQ servers |
| `Callbacks.h` | Callback patterns for async operations |
| `Cancellation.h` | Cancellation token support |

### Specialized Components

| Compat Header | Purpose |
|---------------|---------|
| `MoQPriorityQueue.h` | Priority queue with group expiry for JIT send |
| `Varint.h` | QUIC varint encoding/decoding |
| `BufferAppender.h` | Helper for building buffers |

---

## Architecture Diagrams

### Folly Mode with mvfst/proxygen

```
┌─────────────────────────────────────────────────────────────────┐
│                        Application Layer                         │
│              (MoQDateServer, MoQTextClient, etc.)                │
└───────────────────────────┬─────────────────────────────────────┘
                            │
┌───────────────────────────▼─────────────────────────────────────┐
│                         MoQSession                               │
│  ┌─────────────────┐  ┌──────────────┐  ┌───────────────────┐   │
│  │ Framer/Codec    │  │ Publisher/   │  │ compat::Payload   │   │
│  │ (compat::Cursor)│  │ Subscriber   │  │ (wraps IOBuf)     │   │
│  └─────────────────┘  └──────────────┘  └───────────────────┘   │
│                                                                   │
│  Uses: folly::coro::Task, folly::SemiFuture, folly::IOBufQueue  │
└───────────────────────────┬─────────────────────────────────────┘
                            │ implements proxygen::WebTransportHandler
┌───────────────────────────▼─────────────────────────────────────┐
│                   proxygen::WebTransport                         │
│              (HTTP/3 WebTransport implementation)                │
└───────────────────────────┬─────────────────────────────────────┘
                            │
┌───────────────────────────▼─────────────────────────────────────┐
│                          mvfst                                   │
│                    (QUIC implementation)                         │
│  ┌─────────────────┐  ┌──────────────┐  ┌───────────────────┐   │
│  │ QuicEventBase   │  │ QuicSocket   │  │ fizz (TLS 1.3)    │   │
│  └─────────────────┘  └──────────────┘  └───────────────────┘   │
└─────────────────────────────────────────────────────────────────┘
```

**Key Files:**
- `moxygen/MoQSession.h` - Folly-mode session (lines 25-62)
- `moxygen/compat/ProxygenWebTransportAdapter.h` - Adapter for proxygen types
- `moxygen/events/MoQFollyExecutorImpl.h` - Folly event base integration

### Std Mode with picoquic

```
┌─────────────────────────────────────────────────────────────────┐
│                        Application Layer                         │
│           (PicoDateServer, PicoTextClient, etc.)                 │
└───────────────────────────┬─────────────────────────────────────┘
                            │
┌───────────────────────────▼─────────────────────────────────────┐
│                         MoQSession                               │
│  ┌─────────────────┐  ┌──────────────┐  ┌───────────────────┐   │
│  │ Framer/Codec    │  │ Publisher/   │  │ compat::Payload   │   │
│  │ (ByteCursor)    │  │ Subscriber   │  │ (vector<uint8_t>) │   │
│  └─────────────────┘  └──────────────┘  └───────────────────┘   │
│                                                                   │
│  Uses: compat::SemiFuture, compat::ByteBufferQueue              │
└───────────────────────────┬─────────────────────────────────────┘
                            │ uses compat::WebTransportInterface
┌───────────────────────────▼─────────────────────────────────────┐
│               compat::WebTransportInterface                      │
│          (Abstract transport interface in compat/)               │
└───────────────────────────┬─────────────────────────────────────┘
                            │ implemented by
        ┌───────────────────┴───────────────────┐
        │                                       │
┌───────▼───────────────┐       ┌───────────────▼───────────────┐
│ PicoquicWebTransport  │       │    PicoquicH3Transport        │
│   (Raw QUIC mode)     │       │   (WebTransport over H3)      │
│   ALPN: "moq-00"      │       │      ALPN: "h3"               │
└───────────┬───────────┘       └───────────────┬───────────────┘
            │                                   │
            └───────────────┬───────────────────┘
                            │
┌───────────────────────────▼─────────────────────────────────────┐
│                        picoquic                                  │
│                   (QUIC implementation)                          │
│  ┌─────────────────┐  ┌──────────────┐  ┌───────────────────┐   │
│  │ packet_loop     │  │ picoquic_cnx │  │ picotls (TLS)     │   │
│  └─────────────────┘  └──────────────┘  └───────────────────┘   │
│                                                                   │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │                    h3zero (HTTP/3)                       │    │
│  │  ┌──────────────┐  ┌───────────────┐  ┌──────────────┐  │    │
│  │  │ h3zero_ctx   │  │ pico_webtrans │  │ QPACK codec  │  │    │
│  │  └──────────────┘  └───────────────┘  └──────────────┘  │    │
│  └─────────────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────────────┘
```

**Key Files:**
- `moxygen/MoQSessionCompat.cpp` - Std-mode session implementation
- `moxygen/transports/PicoquicWebTransport.h` - Raw QUIC transport
- `moxygen/transports/PicoquicH3Transport.h` - HTTP/3 WebTransport
- `moxygen/transports/PicoquicMoQClient.h` - Client with URL parsing
- `moxygen/transports/PicoquicMoQServer.h` - Server implementation
- `moxygen/events/MoQSimpleExecutor.h` - Std-mode executor

---

## WebTransportInterface Design

The `WebTransportInterface` (`moxygen/compat/WebTransportInterface.h`) is the key abstraction that decouples MoQSession from the transport layer:

```cpp
class WebTransportInterface {
 public:
  // Stream creation
  virtual Expected<StreamWriteHandle*, WebTransportError> createUniStream() = 0;
  virtual Expected<BidiStreamHandle*, WebTransportError> createBidiStream() = 0;

  // Datagrams
  virtual Expected<Unit, WebTransportError> sendDatagram(Payload data) = 0;

  // Session control
  virtual void closeSession(uint32_t errorCode = 0) = 0;

  // Event callbacks
  virtual void setNewUniStreamCallback(std::function<void(StreamReadHandle*)> cb) = 0;
  virtual void setNewBidiStreamCallback(std::function<void(BidiStreamHandle*)> cb) = 0;
  virtual void setDatagramCallback(std::function<void(Payload)> cb) = 0;
};
```

**Implementations:**

| Implementation | Source File | Transport |
|----------------|-------------|-----------|
| `ProxygenWebTransportAdapter` | `moxygen/compat/ProxygenWebTransportAdapter.h` | proxygen (Folly mode) |
| `PicoquicWebTransport` | `moxygen/transports/PicoquicWebTransport.h` | picoquic raw QUIC |
| `PicoquicH3Transport` | `moxygen/transports/PicoquicH3Transport.h` | picoquic + h3zero |

---

## Async Programming Model

### Folly Mode

Uses folly coroutines and futures:

```cpp
// Coroutine-based (MoQSession.h)
folly::coro::Task<ServerSetup> setup(ClientSetup setup);
folly::coro::Task<SubscribeOk> subscribe(SubscribeRequest req);

// Future-based
folly::SemiFuture<Unit> awaitStreamCredit();
```

### Std Mode

Uses callback-based APIs with Promise/SemiFuture:

```cpp
// Callback-based (MoQSessionCompat.cpp)
void setupWithCallback(
    ClientSetup setup,
    std::shared_ptr<ResultCallback<ServerSetup, Error>> callback);

void subscribeWithCallback(
    SubscribeRequest req,
    std::shared_ptr<TrackConsumer> consumer,
    std::shared_ptr<ResultCallback<SubscriptionHandle, Error>> callback);

// Promise/Future for flow control
compat::SemiFuture<compat::Unit> awaitStreamCredit();
```

The `compat::SemiFuture` and `compat::Promise` (`moxygen/compat/Async.h`) provide:
- Thread-safe value passing
- Continuation support (`.thenValue()`)
- Timeout support
- Exception propagation

---

## Picoquic Integration Details

### Raw QUIC Mode (`PicoquicWebTransport`)

Direct QUIC streams without HTTP/3 framing:

```
Client                                   Server
  │                                        │
  │──────── QUIC Handshake (moq-00) ──────▶│
  │                                        │
  │◀─────── Stream 0 (Bidi Control) ──────▶│
  │         [MoQ SETUP frames]             │
  │                                        │
  │──────── Stream 2 (Uni) ───────────────▶│
  │         [SUBSCRIBE_REQUEST]            │
  │                                        │
  │◀─────── Stream 3 (Uni) ────────────────│
  │         [Object data]                  │
```

**JIT Send API Flow:**
```cpp
// 1. Application queues data
stream->writeStreamData(payload, fin);

// 2. picoquic marks stream active
picoquic_mark_active_stream(cnx, stream_id, 1, stream_ctx);

// 3. picoquic calls prepare_to_send when credits available
int prepare_to_send(stream_id, context) {
    // Signal waiters that we can send
    readyWaiters_.setValue();
    return bytes_available;
}

// 4. picoquic calls provide_stream_data_buffer
uint8_t* provide_stream_data_buffer(length, context) {
    // Return buffer pointer for zero-copy send
    return outBuffer_.data.data();
}
```

### WebTransport Mode (`PicoquicH3Transport`)

HTTP/3 + WebTransport encapsulation for proxygen interop:

```
Client                                   Server
  │                                        │
  │──────── QUIC Handshake (h3) ──────────▶│
  │                                        │
  │──────── HTTP/3 SETTINGS ──────────────▶│
  │◀─────── HTTP/3 SETTINGS ───────────────│
  │                                        │
  │──────── CONNECT :protocol=webtransport▶│
  │         :path=/moq                     │
  │                                        │
  │◀─────── 200 OK ────────────────────────│
  │                                        │
  │◀─────── WT Stream (Bidi Control) ─────▶│
  │         [MoQ SETUP frames]             │
  │                                        │
  │──────── WT Stream (Uni) ──────────────▶│
  │         [SUBSCRIBE_REQUEST]            │
```

**h3zero Callbacks:**
```cpp
// HTTP/3 stream callback
static int h3Callback(cnx, stream_id, bytes, length, event, stream_ctx, ctx);

// WebTransport session callback
static int wtCallback(cnx, bytes, length, event, stream_ctx, ctx);

// Events handled:
// - picohttp_callback_connecting: Connection starting
// - picohttp_callback_connect: CONNECT request received
// - picohttp_callback_connect_refused: Upgrade rejected
// - picohttp_callback_post_data: Data on WT stream
// - picohttp_callback_free: Cleanup
```

---

## Thread Model

### Folly Mode

Uses folly's `EventBase` for event-driven async:

```cpp
class MoQFollyExecutorImpl : public MoQExecutor {
  folly::EventBase* evb_;

  void add(folly::Func func) override {
    evb_->runInEventBaseThread(std::move(func));
  }

  void scheduleTimeout(quic::QuicTimerCallback* cb, milliseconds timeout) override {
    evb_->timer().scheduleTimeout(cb, timeout);
  }
};
```

### Std Mode (Picoquic)

Picoquic runs its own network thread. Cross-thread dispatch via `PicoquicThreadDispatcher`:

```cpp
class PicoquicThreadDispatcher {
  std::deque<std::function<void()>> workQueue_;
  picoquic_network_thread_ctx_t* threadCtx_;

  void runOnPicoThread(std::function<void()> fn) {
    {
      std::lock_guard lock(mutex_);
      workQueue_.push_back(std::move(fn));
    }
    picoquic_wake_up_network_thread(threadCtx_);
  }

  // Called from picoquic_packet_loop_wake_up event
  void drainWorkQueue() {
    std::deque<std::function<void()>> work;
    {
      std::lock_guard lock(mutex_);
      work.swap(workQueue_);
    }
    for (auto& fn : work) {
      fn();
    }
  }
};
```

Application-level work runs on `MoQSimpleExecutor`:

```cpp
class MoQSimpleExecutor : public MoQExecutor {
  std::deque<std::function<void()>> tasks_;

  void run() {  // Main event loop
    while (running_) {
      std::function<void()> task;
      {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [this] { return !tasks_.empty() || !running_; });
        if (!running_) break;
        task = std::move(tasks_.front());
        tasks_.pop_front();
      }
      task();
    }
  }
};
```

---

## Buffer Management

### Folly Mode

Uses `folly::IOBuf` chain for zero-copy operations:

```cpp
// IOBuf provides:
// - Reference-counted shared buffers
// - Chain of non-contiguous buffers
// - Zero-copy slicing and sharing
// - Headroom/tailroom for efficient prepend/append

std::unique_ptr<folly::IOBuf> buf = folly::IOBuf::create(capacity);
buf->append(length);  // Extend valid data region
buf->trimStart(n);    // Consume bytes from front
```

### Std Mode

Uses `ByteBuffer` with Small Buffer Optimization (SBO):

```cpp
// ByteBuffer (moxygen/compat/ByteBuffer.h)
class ByteBuffer {
  static constexpr size_t kInlineSize = 64;  // SBO threshold

  union Storage {
    std::array<uint8_t, kInlineSize> inline_;  // Small buffers
    struct { uint8_t* data; size_t capacity; } heap_;  // Large buffers
  } storage_;

  size_t offset_{0};   // Headroom for O(1) trimStart
  size_t length_{0};   // Valid data length
  bool isInline_{true};

  void trimStart(size_t n) {
    offset_ += n;  // O(1) - just adjust offset
    length_ -= n;
  }
};
```

`ByteBufferQueue` provides chain operations:

```cpp
// ByteBufferQueue (moxygen/compat/ByteBufferQueue.h)
class ByteBufferQueue {
  std::deque<std::unique_ptr<ByteBuffer>> chain_;

  // O(1) for single buffer
  std::unique_ptr<ByteBuffer> move() {
    if (chain_.size() == 1) {
      return std::move(chain_.front());  // No copy!
    }
    return moveCoalesced();  // Copy only when multiple buffers
  }
};
```

---

## Hash Functions

The `Hash.h` (`moxygen/compat/Hash.h`) provides multiple hash implementations:

| Hash Type | Use Case | Algorithm |
|-----------|----------|-----------|
| `SecureStringHash` | String keys in maps | SipHash-2-4 |
| `TransparentStringHash` | Heterogeneous lookup | SipHash-2-4 |
| `IntegerHash` | Integer keys | splitmix64 |
| `FnvHash` | Fast non-crypto hash | FNV-1a |
| `WyHash` | Very fast general hash | wyhash |
| `constexprHash` | Compile-time strings | FNV-1a |
| `PairHash` / `TupleHash` | Composite keys | hash_combine |
| `EnumHash` | Enum keys | mixHash |

---

## Source File Reference

### Compat Layer (`moxygen/compat/`)

| File | Lines | Description |
|------|-------|-------------|
| `Config.h` | 44 | Build configuration macros |
| `Payload.h` | 242 | Buffer abstraction (IOBuf/vector) |
| `ByteBuffer.h` | 352 | Std-mode buffer with SBO |
| `ByteBufferQueue.h` | 382 | Std-mode buffer queue |
| `ByteCursor.h` | ~350 | Std-mode read cursor |
| `BufferIO.h` | 42 | Type aliases for buffers |
| `Async.h` | ~400 | SemiFuture/Promise implementation |
| `Expected.h` | ~200 | Error handling type |
| `Try.h` | ~150 | Exception wrapper |
| `Containers.h` | ~1010 | Hash map/set implementations |
| `Hash.h` | ~500 | Hash function utilities |
| `WebTransportInterface.h` | 267 | Abstract transport interface |
| `Logging.h` | 338 | spdlog-based logging |
| `Debug.h` | 159 | CHECK/DCHECK macros |
| `SocketAddress.h` | ~280 | Network address |
| `Url.h` | ~380 | URL parser |
| `Callbacks.h` | ~100 | Callback patterns |
| `Cancellation.h` | ~100 | Cancellation tokens |
| `MoQPriorityQueue.h` | ~250 | Priority queue with expiry |

### Transport Layer (`moxygen/transports/`)

| File | Lines | Description |
|------|-------|-------------|
| `PicoquicWebTransport.h` | ~180 | Raw QUIC transport |
| `PicoquicWebTransport.cpp` | ~400 | Implementation |
| `PicoquicH3Transport.h` | ~330 | HTTP/3 WebTransport |
| `PicoquicH3Transport.cpp` | ~600 | Implementation |
| `PicoquicMoQClient.h` | ~185 | Client with factory |
| `PicoquicMoQClient.cpp` | ~480 | Implementation |
| `PicoquicMoQServer.h` | ~150 | Server implementation |
| `PicoquicMoQServer.cpp` | ~500 | Implementation |
| `TransportMode.h` | ~30 | QUIC vs WebTransport enum |

### Event Loop (`moxygen/events/`)

| File | Description |
|------|-------------|
| `MoQExecutor.h` | Abstract executor interface |
| `MoQFollyExecutorImpl.h` | Folly EventBase executor |
| `MoQSimpleExecutor.h` | Std-mode simple executor |
| `MoQLibevExecutorImpl.h` | libev-based executor |

---

## Adding New Compat Types

To add a new type that needs mode-specific implementations:

1. Create header in `moxygen/compat/`
2. Use `#if MOXYGEN_USE_FOLLY` for conditional compilation
3. In Folly mode: wrap or alias the folly type
4. In Std mode: implement from scratch using C++20
5. Export via `namespace moxygen::compat`

Example pattern:

```cpp
// moxygen/compat/NewType.h
#pragma once
#include <moxygen/compat/Config.h>

#if MOXYGEN_USE_FOLLY
#include <folly/NewType.h>
#endif

namespace moxygen::compat {

#if MOXYGEN_USE_FOLLY
using NewType = folly::NewType;
#else
class NewType {
  // Pure C++20 implementation
};
#endif

} // namespace moxygen::compat
```

---

## Performance Considerations

### Folly Mode Advantages
- `folly::IOBuf`: Zero-copy buffer chains, reference counting
- `folly::F14FastMap`: SIMD-optimized hash tables
- `folly::coro`: Efficient stackless coroutines
- Mature, battle-tested code

### Std Mode Optimizations
- `ByteBuffer`: SBO avoids allocation for small buffers (< 64 bytes)
- `ByteBuffer`: O(1) trimStart via offset adjustment
- `ByteBufferQueue`: Single-buffer fast path (no copy)
- `RobinHoodMap`: Cache-friendly open addressing
- `WyHash`: Very fast hash for general use
- `SipHash`: Security against hash flooding

---

## Testing

Run std-mode server and client:

```bash
# Build std-mode
cmake -B _build_std -DMOXYGEN_USE_FOLLY=OFF -DMOXYGEN_QUIC_BACKEND=picoquic
cmake --build _build_std

# Run date server (raw QUIC)
./_build_std/moxygen/samples/date/picodateserver -p 4433 \
  --cert cert.pem --key key.pem -n moq-date -m spg -t quic

# Run text client
./_build_std/moxygen/samples/text-client/picotextclient \
  -H localhost -p 4433 -n moq-date --track date -T quic -k
```

For WebTransport interop with proxygen server:

```bash
# Server uses -t webtransport
./_build_std/moxygen/samples/date/picodateserver -p 4433 \
  --cert cert.pem --key key.pem -n moq-date -m spg -t webtransport

# Client uses -T webtransport
./_build_std/moxygen/samples/text-client/picotextclient \
  -H localhost -p 4433 -n moq-date --track date -T webtransport -k
```
