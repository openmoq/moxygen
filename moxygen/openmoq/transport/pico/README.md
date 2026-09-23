# PicoQuic Transport for MOQT

This directory contains the picoquic-based QUIC transport backend for MOQT,
supporting both QUIC transport (non-browser clients) and HTTP/3 WebTransport (browsers).

---

## Architecture Overview

```
┌─────────────────────────────┐               ┌─────────────────────────────┐
│  MoQPicoQuicServer          │               │ MoQPicoQuicEventBaseServer  │
│  (Threaded Model)           │               │ (EventBase Model)           │
│                             │               │                             │
│ • picoquic_start_network_   │               │ • PicoQuicSocketHandler     │
│   thread()                  │               │ • folly::EventBase          │
│ • PicoQuicExecutor          │               │ • MoQFollyExecutorImpl      │
│ • Single dedicated thread   │               │ • Shared event loop         │
└─────────────────────────────┘               └─────────────────────────────┘
              │                                               │
              │                               MoQPicoQuicShardedServer owns
              │                               N of these, one per EventBase,
              │                               on a shared SO_REUSEPORT address
              └───────────────────────┬───────────────────────┘
                                      │  owns
                                      ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                               picoquic                                      │
│                    (QUIC protocol, congestion control)                      │
└─────────────────────────────────────────────────────────────────────────────┘
                                      │
                                      ▼
                            ┌─────────────────┐
                            │   UDP Socket    │
                            └─────────────────┘

On each connection, picoquic drives one of two WebTransport adapters:

┌─────────────────────────────┐               ┌─────────────────────────────┐
│   PicoQuicWebTransport      │               │    PicoH3WebTransport       │
│   (QUIC - moqt-NN)          │               │    (HTTP/3 WebTransport)    │
│                             │               │                             │
│ • picoquic_callback_*       │               │ • picohttp_callback_*       │
│ • Direct stream IDs         │               │ • h3zero stream contexts    │
│ • Native QUIC clients       │               │ • Browser clients           │
└─────────────────────────────┘               └─────────────────────────────┘
              │                                               │
              └───────────────────────┬───────────────────────┘
                                      │  inherits
                                      ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                          PicoWebTransportBase                               │
│           (shared: WtStreamManager, JIT send, egress events)                │
└─────────────────────────────────────────────────────────────────────────────┘
                                      │  implements
                                      ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                        proxygen::WebTransport Interface                     │
│              (streams, datagrams, flow control, session mgmt)               │
└─────────────────────────────────────────────────────────────────────────────┘
                                      │
                                      ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                             MOQT Application                                │
│                         (MoQSession, MoQRelay, etc.)                        │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## Class Hierarchy

### Server Classes

```
MoQPicoServerBase                    <- Shared: QUIC context, ALPN, h3zero init
    │
    ├── MoQPicoQuicServer            <- Threaded: picoquic_start_network_thread
    │       └── PicoQuicExecutor     <- Executor for coroutines on packet thread
    │
    └── MoQPicoQuicEventBaseServer   <- EventBase: PicoQuicSocketHandler
            └── PicoQuicSocketHandler <- UDP I/O, wake timer on EventBase
            └── MoQFollyExecutorImpl  <- Executor backed by EventBase

MoQServerBase
    └── MoQPicoQuicShardedServer     <- N EventBase servers on one address
            └── ShardServer (xN)     <- MoQPicoQuicEventBaseServer per EventBase
```

### Client Classes

```
MoQPicoQuicEventBaseClient           <- Outgoing connection on a caller's EventBase
    └── PicoQuicSocketHandler        <- Same UDP I/O engine as the EVB server
```

### WebTransport Adapters

```
proxygen::WebTransport (interface)
        │
PicoWebTransportBase                 <- Shared base: WtStreamManager, JIT, egress
   ├── PicoQuicWebTransport          <- QUIC transport (moqt-NN ALPN)
   └── PicoH3WebTransport            <- HTTP/3 WebTransport (h3 ALPN)
```

---

## PicoWebTransportBase

The base class implements the full `proxygen::WebTransport` interface and
provides shared functionality for both transport variants:

### Shared Implementation

| Component | Description |
|-----------|-------------|
| `WtStreamManager` | Per-stream buffering, read/write handles, priority queue |
| `processEgressEvents()` | Drains WtStreamManager events (reset, stop-sending, close) |
| `onJitProvideData()` | JIT send path - dequeues data and provides to picoquic |
| `onStreamDataCommon()` | Ingress data delivery with deferred stream notification |
| `WakeTimeGuard` | RAII helper to reschedule wake timer on state changes |

### Pure Virtual Primitives (Subclass Implements)

| Method | PicoQuicWebTransport | PicoH3WebTransport |
|--------|---------------------|-------------------|
| `createStreamImpl()` | `picoquic_get_next_local_stream_id` | `picowt_create_local_stream` |
| `markStreamActiveImpl()` | `picoquic_mark_active_stream(id, nullptr)` | `picoquic_mark_active_stream(id, streamCtx)` |
| `markDatagramActiveImpl()` | `picoquic_mark_datagram_ready` | `h3zero_set_datagram_ready` |
| `resetStreamImpl()` | `picoquic_reset_stream` | `picowt_reset_stream` |
| `stopSendingImpl()` | `picoquic_stop_sending` | `picoquic_stop_sending` |
| `sendCloseImpl()` | `picoquic_close` | `picowt_send_close_session_message` |

---

## Protocol Selection (ALPN)

| ALPN | Protocol | WebTransport Adapter |
|------|----------|---------------------|
| `moqt-16`, `moqt-15`, etc. | QUIC transport | `PicoQuicWebTransport` |
| `h3` | HTTP/3 WebTransport | `PicoH3WebTransport` |

ALPN preference order: MOQT ALPNs first (preferred for non-browser clients),
`h3` last (fallback for browsers).

---

## Thread Models

### Threaded Model (MoQPicoQuicServer)

A single network thread spawned by `picoquic_start_network_thread`. All picoquic
I/O, callbacks, and coroutines run on this thread. `PicoQuicExecutor` integrates
coroutine execution into the packet loop.

```
┌────────────────────────────────────────────────────────────────┐
│                    Packet Loop Thread                          │
│  ┌──────────────────────────────────────────────────────────┐  │
│  │  picoquic_packet_loop()                                  │  │
│  │    • select/epoll on UDP socket                          │  │
│  │    • picoquic_incoming_packet() for received data        │  │
│  │    • picoquic_prepare_next_packet_ex() for outgoing      │  │
│  │    • Invokes picoCallback / h3zero_callback              │  │
│  └──────────────────────────────────────────────────────────┘  │
│                              │                                 │
│                              ▼                                 │
│  ┌──────────────────────────────────────────────────────────┐  │
│  │  PicoQuicExecutor (loopCallbackStatic)                   │  │
│  │    • Drains pending tasks (folly::Func)                  │  │
│  │    • Processes expired timers                            │  │
│  │    • Runs MoQSession coroutines                          │  │
│  └──────────────────────────────────────────────────────────┘  │
└────────────────────────────────────────────────────────────────┘
```

### EventBase Model (MoQPicoQuicEventBaseServer)

Caller supplies a `folly::EventBase`. `PicoQuicSocketHandler` drives picoquic I/O
via `AsyncUDPSocket` in notify-only mode. It reads with a hand-built `recvmmsg`
that captures `IP_PKTINFO` and TOS/ECN, and sends with batched `sendmmsg` (see
[UDP Send Path](#udp-send-path)). The wake timer runs on a dedicated
`STTimerFDTimeoutManager`, because `EventBase::scheduleTimeoutHighRes()` rounds
up to whole milliseconds, too coarse for picoquic's microsecond pacing. A
zero-delay wake runs as an owned `LoopCallback` so `stop()` can cancel it.

`UDP_GRO` stays off: the receive path reads one datagram per buffer and does not
split coalesced trains.

```
┌────────────────────────────────────────────────────────────────┐
│                    folly::EventBase Thread                     │
│  ┌──────────────────────────────────────────────────────────┐  │
│  │  EventBase::loopForever()                                │  │
│  └──────────────────────────────────────────────────────────┘  │
│              │                               │                 │
│  ┌───────────▼─────────────┐   ┌─────────────▼───────────────┐ │
│  │ PicoQuicSocketHandler   │   │ MoQFollyExecutorImpl        │ │
│  │  • onNotifyDataAvailable│   │  • Runs MoQSession coros    │ │
│  │  • recvmmsg batching    │   │  • Handles timers           │ │
│  │  • sendmmsg + GSO       │   │                             │ │
│  │  • timerfd wake timer   │   │                             │ │
│  └─────────────────────────┘   └─────────────────────────────┘ │
└────────────────────────────────────────────────────────────────┘
```

### Sharded Model (MoQPicoQuicShardedServer)

`MoQPicoQuicShardedServer` spreads one listener across N EventBases. It owns one
independent `MoQPicoQuicEventBaseServer` per EventBase, each with its own
`picoquic_quic_t` and UDP socket, all bound to the same address with
`SO_REUSEPORT`. The kernel load-balances incoming packets by 4-tuple hash.

- `start(addr, evbs)` binds one shard per EventBase. An empty `evbs` spins up
  one internally owned thread. The first shard binds the port (resolving port 0)
  and the rest join its reuseport group.
- Each shard forwards `createSession`, `onNewSession`,
  `terminateClientSession` and `makeServerSetup` to the parent, so a subclass of
  `MoQPicoQuicEventBaseServer` moves over with a base-class swap.
- With more than one shard, QUIC connection migration is forced off:
  `SO_REUSEPORT` cannot route a migrated connection's packets to the shard that
  holds its state. A single shard behaves like `MoQPicoQuicEventBaseServer`.
- Each shard's picoquic context is pinned to its EventBase thread. Debug and
  sanitizer builds (`WITH_THREAD_CHECK`) turn on picoquic's thread check, which
  aborts on a cross-thread access.
- `setPicoQuicStatsCallbackFactory()` builds one stats callback per shard, on
  that shard's EventBase.
- `stop()` drains every shard on its own EventBase thread and blocks until the
  last session is gone. Call it from outside the shard threads.

---

## UDP Send Path

`PicoQuicSocketHandler::drainOutgoing()` pulls packets from picoquic with
`picoquic_prepare_next_packet_ex()` into a preallocated `SendBatch` and flushes
it with a single `sendmmsg`. The send path does not allocate.

- Packets land back-to-back in an arena. Consecutive packets that share a
  destination, source address and interface coalesce into one `mmsghdr` slot
  sent with `UDP_SEGMENT` (GSO). Only the last segment in a slot may be short.
- Each slot carries its own `IP_PKTINFO` cmsg for per-packet source address
  control. folly's `writem`/`writemGSO` cannot express this, so the `mmsghdr`
  array is hand-built, mirroring the receive path.
- On `EAGAIN` or a short `sendmmsg` count, the unsent slots stay in the batch
  and the handler registers for `EPOLLOUT`. The drain resumes when the socket
  becomes writable, rather than spinning.
- A non-retryable error drops that one slot and continues. `EIO` on a segmented
  send disables GSO for the socket, since the driver rejects `UDP_SEGMENT`.
- Send counters (calls, messages, datagrams, `EAGAIN`, drops) are logged at
  `stop()`.

Batch limits live in `PicoSocketConfig` (`PicoTransportConfig::socket`):

| Field | Default | Meaning |
|-------|---------|---------|
| `maxMsgsPerBatch` | 32 | `mmsghdr` slots per `sendmmsg` |
| `maxBytesPerBatch` | 64 KB | Arena bytes per batch |
| `maxSegmentsPerMsg` | 32 | GSO segments per slot |
| `maxBytesPerMsg` | 45000 | Bytes per GSO slot |
| `maxPacketsPerDrain` | 64 | Packets pulled per drain before yielding to the EventBase |
| `socketBufferBytes` | 1 MB | `SO_SNDBUF`/`SO_RCVBUF` (clamped by `wmem_max`/`rmem_max`) |

Raising these trades EventBase responsiveness for fewer syscalls.

---

## JIT (Just-In-Time) Send Model

Both transport variants use picoquic's JIT send model - data is not pushed
proactively but provided on demand.

### Stream Writes

```
1. Application writes:
   MoQSession -> writeStreamData(id, IOBuf, fin)
     -> WtStreamManager buffers data
     -> markStreamActiveImpl(id)  // signals picoquic

2. Picoquic calls back when ready:
   picoquic_callback_prepare_to_send (QUIC) / picohttp_callback_provide_data (H3)
     -> PicoWebTransportBase::onJitProvideData(streamId, context, maxLength)
          -> streamManager_->dequeue(*handle, maxLength)
          -> picoquic_provide_stream_data_buffer(context, dataLen, fin, isActive)
          -> memcpy data into returned buffer
          -> Fire delivery callback (optimistic)
```

Only one stream is marked active with picoquic at a time. When it drains,
`markNextWritableStreamActive()` hands picoquic the head of `WtStreamManager`'s
writable queue. `WtStreamManager`'s `eventsAvailable` is edge-triggered and does
not fire while streams stay queued through a burst, so the JIT path also calls
`drainEgressEvents()` to dispatch control events (reset, stop-sending, close)
queued mid-burst.

### Datagram Writes

```
1. Application queues:
   sendDatagram(IOBuf) -> datagramQueue_.push_back() -> markDatagramActiveImpl()

2. Picoquic calls back:
   picoquic_callback_prepare_datagram / picohttp_callback_provide_datagram
     -> Dequeue and copy to picoquic buffer
```

---

## Wake Timeout Optimization

When `markStreamActive` or `markDatagramActive` is called, picoquic's next wake
time may decrease. The `WakeTimeGuard` RAII helper captures the wake time before
and after, invoking `updateWakeTimeoutCallback` if it decreased. This ensures
the EventBase timer is rescheduled promptly, avoiding latency spikes.

---

## HTTP/3 WebTransport Specifics (PicoH3WebTransport)

### h3zero Callback Flow

```
Browser HTTP/3 CONNECT
  -> h3zero processes HTTP/3 frames
  -> wtPathCallback(picohttp_callback_connect, ...)
  -> MoQPicoServerBase creates PicoH3WebTransport
  -> 200 OK sent to browser

Subsequent events:
  -> h3zero decodes HTTP/3 frames
  -> wtPathCallback(picohttp_callback_*, ...)
  -> PicoH3WebTransport::handleWtEvent(...)
```

### Control Stream

HTTP/3 WebTransport uses a dedicated control stream for session management
capsules (`CLOSE_WEBTRANSPORT_SESSION`, `DRAIN_WEBTRANSPORT_SESSION`).

### Session Context

`PicoH3SessionContext` (in `PicoConnectionContext.h`) is stored in
`streamCtx->path_callback_ctx` and holds the `PicoH3WebTransport` and
`MoQSession`. `dispatchH3Event` routes all `picohttp_callback_*` events by
casting and checking the magic field. When `handleWtEvent` returns `kDeleteCtx`
(all streams freed after deregister), `dispatchH3Event` deletes the session
context.

### Stream Context Tracking

Unlike QUIC transport, H3 requires tracking `h3zero_stream_ctx_t*` per stream for
JIT callbacks. Stored in `streamContexts_` map. New streams must inherit
`path_callback` from the control stream context.

### Stream Lifecycle

`maybeDeleteStream` calls `h3zero_delete_stream` when both `is_fin_received`
and `is_fin_sent` are set on a stream context, prompting h3zero to free the
stream promptly rather than waiting until connection close. Triggered from
`post_fin`, `reset` (incoming), egress FIN (`provide_data`), and
`resetStreamImpl`.

---

## Connection Lifecycle

### QUIC transport (moqt-NN ALPN)

1. `picoquic_callback_almost_ready` — ALPN confirmed as QUIC transport; no callback switch needed
2. `picoquic_callback_ready` → `onNewConnectionImpl` creates `PicoQuicWebTransport` + `MoQSession`
3. Session coroutine starts via executor

### HTTP/3 WebTransport (h3 ALPN)

1. `picoquic_callback_almost_ready` — ALPN confirmed as `h3`; picoquic callback switched to `h3zero_callback`, `almost_ready` forwarded to h3zero
2. `picoquic_callback_ready` forwarded to h3zero; `onNewConnectionImpl` is a no-op for h3 connections
3. Browser sends HTTP/3 CONNECT → `wtPathCallback(picohttp_callback_connect)` → `onWebTransportConnectImpl` creates `PicoH3WebTransport` + `MoQSession`
4. Session coroutine starts via executor

### Close

| Trigger | Path |
|---------|------|
| Peer closes | Callback → `onSessionCloseCommon` → handler notification |
| Local close | `closeSession()` → `sendCloseImpl()` → handler notification → drain |

The close callbacks log picoquic's local, remote and application error codes and
close reasons at `DBG1`, which tells a peer-initiated close from a local one.

---

## Configuration

### PicoTransportConfig

QUIC transport parameters applied to each `picoquic_quic_t` (server and client):

| Field | Default | Notes |
|-------|---------|-------|
| `maxData` / `maxStreamData` | 64 MB / 16 MB | Connection and per-stream flow control |
| `maxUniStreams` / `maxBidiStreams` | 8192 / 16 | Stream limits |
| `maxDatagramFrameSize` | 1280 | |
| `idleTimeoutMs` | 30000 | Handshake timeout is half |
| `maxAckDelayUs` / `minAckDelayUs` | 100000 / 1000 | |
| `disableMigration` | false | Forced on by the sharded server with >1 shard |
| `ccAlgo` | `bbr` | Congestion control algorithm |
| `mtuMax` | 1500 | Real link MTU. picoquic subtracts IP/UDP overhead from it so PMTU probes fit |
| `socket` | `PicoSocketConfig{}` | Send-path tuning, see [UDP Send Path](#udp-send-path) |

The client reads the MTU from `--pico_mtu_max` (default 1500).

### PicoWebTransportConfig

| Field | Default | Notes |
|-------|---------|-------|
| `enableQuicTransport` | true | Offer `moqt-NN` ALPNs |
| `enableWebTransport` | false | Offer `h3` for browsers |
| `wtEndpoints` | `{"/moq"}` | CONNECT paths. h3zero matches up to `?`, so `/moq` does not match `/moq/relay` |
| `wtMaxSessions` | 100 | Concurrent WebTransport sessions |

### Dual-stack

A listener bound to `::` accepts IPv4 peers: the bind clears `IPV6_V6ONLY`, and
IPv4 addresses are promoted to IPv4-mapped form.

---

## Stats and Logging

`PicoQuicStatsCallback` reports connection, stream, packet and path-quality
events (RTT, receive rate, bytes in transit). Register one with
`MoQPicoServerBase::setPicoQuicStatsCallback()`, or per shard with
`MoQPicoQuicShardedServer::setPicoQuicStatsCallbackFactory()`. Calls arrive on
the server's EventBase thread. For `h3` connections, session counts track
WebTransport sessions rather than connections. Path quality currently reaches
raw QUIC connections only, because h3zero takes over the picoquic callback.

`installPicoQuicXLogSink(quic)` (`PicoQuicXLogSink.h`) routes picoquic's internal
log events through folly XLOG under the `quic.picoquic.*` category, controlled by
the usual `--logging=` config.

---

## Samples

| Binary | Class | Description |
|--------|-------|-------------|
| `pico_relay_server` | `MoQPicoQuicServer` | Thread-based MOQT relay |
| `pico_evb_relay_server` | `MoQPicoQuicEventBaseServer` | EventBase MOQT relay |
| `pico_evb_text_client` | `MoQPicoQuicEventBaseClient` | EventBase text subscriber |

### Running

```bash
# Thread-based relay (default port 9668)
./bin/pico_relay_server --port 9668 --cert cert.pem --key key.pem

# EventBase relay, with browser WebTransport on /moq
./bin/pico_evb_relay_server --port 9668 --cert cert.pem --key key.pem \
    --enable_webtransport --wt_endpoint /moq

# Text client
./bin/pico_evb_text_client --connect_url moqt://localhost:9668/moq-relay \
    --track_namespace ns --track_name track
```

---

## Tests

`test/PicoWebTransportBaseTest.cpp` covers the JIT egress path in
`PicoWebTransportBase`, including control events queued mid-burst.

---

## Files

| File | Description |
|------|-------------|
| `PicoWebTransportBase.h/cpp` | Shared WebTransport base class |
| `PicoQuicWebTransport.h/cpp` | QUIC transport WebTransport adapter |
| `PicoH3WebTransport.h/cpp` | HTTP/3 WebTransport adapter |
| `PicoConnectionContext.h` | Per-connection/session context structs and dispatch helpers |
| `PicoProtocolDispatcher.h` | ALPN → PicoProtocolType mapping |
| `MoQPicoServerBase.h/cpp` | Shared server base (ALPN, h3zero init) |
| `MoQPicoQuicServer.h/cpp` | Threaded server |
| `MoQPicoQuicEventBaseServer.h/cpp` | EventBase server |
| `MoQPicoQuicShardedServer.h/cpp` | EventBase server sharded across N EventBases |
| `MoQPicoQuicEventBaseClient.h/cpp` | EventBase client |
| `PicoQuicSocketHandler.h/cpp` | EventBase UDP I/O engine (recvmmsg/sendmmsg, wake timer) |
| `PicoQuicExecutor.h/cpp` | Thread-based executor |
| `PicoTransportConfig.h` | Transport, WebTransport and socket config structs |
| `PicoQuicStatsCallback.h` | Transport stats callback interface |
| `PicoQuicXLogSink.h/cpp` | Routes picoquic logs through folly XLOG |
