# PicoQuic WebTransport Architecture

This document describes the architecture, callback flow, and state management
for HTTP/3 WebTransport support in the picoquic transport backend.

---

## System Architecture

The picoquic stack supports two I/O models (threaded and EventBase) and two
protocol modes (raw QUIC and HTTP/3 WebTransport):

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              MoQ Application                                │
│                         (MoQSession, MoQRelay, etc.)                        │
└─────────────────────────────────────────────────────────────────────────────┘
                                      │
                                      ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                        proxygen::WebTransport Interface                     │
│              (streams, datagrams, flow control, session mgmt)               │
└─────────────────────────────────────────────────────────────────────────────┘
                                      │
              ┌───────────────────────┴───────────────────────┐
              │                                               │
              ▼                                               ▼
┌─────────────────────────────┐               ┌─────────────────────────────┐
│   PicoQuicWebTransport      │               │    PicoH3WebTransport       │
│   (Raw QUIC - moqt-NN)      │               │    (HTTP/3 WebTransport)    │
│                             │               │                             │
│ • picoquic_callback_*       │               │ • picohttp_callback_*       │
│ • Direct stream IDs         │               │ • h3zero stream contexts    │
│ • Native QUIC clients       │               │ • Browser clients           │
└─────────────────────────────┘               └─────────────────────────────┘
              │                                               │
              └───────────────────────┬───────────────────────┘
                                      │
                                      ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                          WtStreamManager (proxygen)                         │
│            (per-stream buffering, read/write handles, priority queue)       │
└─────────────────────────────────────────────────────────────────────────────┘
                                      │
                                      ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                               picoquic                                      │
│                    (QUIC protocol, congestion control)                      │
└─────────────────────────────────────────────────────────────────────────────┘
                                      │
              ┌───────────────────────┴───────────────────────┐
              │                                               │
              ▼                                               ▼
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
              └───────────────────────┬───────────────────────┘
                                      │
                                      ▼
                            ┌─────────────────┐
                            │   UDP Socket    │
                            └─────────────────┘
```

### Server Class Hierarchy

```
MoQPicoServerBase                    <- Shared: QUIC context, ALPN, h3zero init
    │
    ├── MoQPicoQuicServer            <- Threaded: picoquic_start_network_thread
    │       └── PicoQuicExecutor     <- Executor for coroutines on packet thread
    │
    └── MoQPicoQuicEventBaseServer   <- EventBase: PicoQuicSocketHandler
            └── PicoQuicSocketHandler <- UDP I/O, wake timer on EventBase
            └── MoQFollyExecutorImpl  <- Executor backed by EventBase

WebTransport Adapters (per-connection):
    ├── PicoQuicWebTransport         <- Raw QUIC (moqt-NN ALPN)
    └── PicoH3WebTransport           <- HTTP/3 WebTransport (h3 ALPN)
```

---

## Protocol Selection (ALPN)

The server selects protocol based on TLS ALPN negotiation:

| ALPN | Protocol | WebTransport Adapter |
|------|----------|---------------------|
| `moqt-16`, `moqt-15`, etc. | Raw MoQ over QUIC | `PicoQuicWebTransport` |
| `h3` | HTTP/3 WebTransport | `PicoH3WebTransport` |

ALPN preference order: raw MoQ ALPNs first (preferred for native clients),
`h3` last (fallback for browsers).

---

## Threaded Model (MoQPicoQuicServer)

The threaded model uses picoquic's built-in network thread:

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
│                              │                                  │
│                              ▼                                  │
│  ┌──────────────────────────────────────────────────────────┐  │
│  │  PicoQuicExecutor (loopCallbackStatic)                   │  │
│  │    • Drains pending tasks (folly::Func)                  │  │
│  │    • Processes expired timers                            │  │
│  │    • Runs MoQSession coroutines                          │  │
│  └──────────────────────────────────────────────────────────┘  │
└────────────────────────────────────────────────────────────────┘

Executor Integration Points:
  • picoquic_packet_loop_ready      -> Enable time check
  • picoquic_packet_loop_time_check -> Set delta_t based on pending tasks/timers
  • picoquic_packet_loop_after_receive -> drainTasks(), processExpiredTimers()
  • picoquic_packet_loop_after_send    -> drainTasks(), processExpiredTimers()
```

### Threaded WebTransport Flow

For HTTP/3 WebTransport on the threaded model:

```
Browser CONNECT arrives
    │
    ▼
picoquic_packet_loop receives UDP packet
    │
    ▼
picoquic_incoming_packet() decodes QUIC
    │
    ▼
h3zero_callback() decodes HTTP/3
    │
    ▼
wtPathCallback(picohttp_callback_connect, ...)
    │
    ▼
MoQPicoServerBase::onWebTransportConnectImpl()
    ├── Creates PicoH3WebTransport
    ├── Creates MoQSession
    └── Starts session coroutine via PicoQuicExecutor
            │
            ▼ (on next packet loop iteration)
    PicoQuicExecutor::drainTasks()
            │
            ▼
    MoQSession coroutine runs on packet thread
```

**Key point**: All callbacks AND coroutines run on the single packet loop thread.
`PicoQuicExecutor` integrates coroutine execution into the packet loop.

---

## EventBase Model (MoQPicoQuicEventBaseServer)

The EventBase model drives picoquic from a folly EventBase:

```
┌────────────────────────────────────────────────────────────────┐
│                    folly::EventBase Thread                     │
│  ┌──────────────────────────────────────────────────────────┐  │
│  │  EventBase::loopForever()                                │  │
│  │    • Runs registered callbacks                           │  │
│  │    • Handles AsyncTimeout expiry                         │  │
│  │    • Processes AsyncUDPSocket events                     │  │
│  └──────────────────────────────────────────────────────────┘  │
│                              │                                  │
│              ┌───────────────┴───────────────┐                  │
│              ▼                               ▼                  │
│  ┌─────────────────────────┐   ┌─────────────────────────────┐ │
│  │ PicoQuicSocketHandler   │   │ MoQFollyExecutorImpl        │ │
│  │  (AsyncUDPSocket)       │   │  (schedule via EventBase)   │ │
│  │  • onNotifyDataAvailable│   │  • Runs MoQSession coros    │ │
│  │  • recvmmsg batching    │   │  • Handles timers           │ │
│  │  • sendmsg with GSO     │   │                             │ │
│  └─────────────────────────┘   └─────────────────────────────┘ │
│              │                               │                  │
│              ▼                               │                  │
│  ┌─────────────────────────┐                │                  │
│  │ AsyncTimeout            │                │                  │
│  │  • rescheduleTimer()    │<───────────────┘                  │
│  │  • timeoutExpired()     │   (wake time updates)             │
│  └─────────────────────────┘                                   │
└────────────────────────────────────────────────────────────────┘
```

### EventBase WebTransport Flow

For HTTP/3 WebTransport on the EventBase model:

```
Browser CONNECT arrives
    │
    ▼
AsyncUDPSocket::onNotifyDataAvailable()
    │
    ▼
PicoQuicSocketHandler::onNotifyDataAvailable()
    ├── recvmmsg() batches packets
    └── picoquic_incoming_packet() for each
            │
            ▼
    h3zero_callback() decodes HTTP/3
            │
            ▼
    wtPathCallback(picohttp_callback_connect, ...)
            │
            ▼
    MoQPicoServerBase::onWebTransportConnectImpl()
        ├── Creates PicoH3WebTransport
        ├── Creates MoQSession
        └── Starts session coroutine via MoQFollyExecutorImpl
                │
                ▼
        EventBase::runInEventBaseThread(coroutine)
                │
                ▼ (same EventBase iteration or next)
        MoQSession coroutine runs on EventBase thread
```

---

## PicoH3WebTransport Details

`PicoH3WebTransport` provides WebTransport over HTTP/3 using picoquic's h3zero
library. This enables browser clients to connect via standard HTTP/3 WebTransport.

### Raw QUIC vs HTTP/3

| Aspect | PicoQuicWebTransport | PicoH3WebTransport |
|--------|---------------------|-------------------|
| Protocol | Raw QUIC | HTTP/3 WebTransport |
| Callback source | `picoquic_callback_*` | `picohttp_callback_*` via h3zero |
| Stream creation | `picoquic_get_next_local_stream_id` | `picowt_create_local_stream` |
| Datagram API | `picoquic_mark_datagram_ready` | `h3zero_set_datagram_ready` |
| Control stream | None | WebTransport control stream (capsules) |
| Client support | Native QUIC clients | Browsers, native clients |

### Class Hierarchy

```
MoQPicoH3EventBaseServer           <- HTTP/3 WebTransport server using EventBase
    ├── PicoQuicSocketHandler      <- UDP I/O (shared with raw QUIC)
    └── PicoH3WebTransport         <- WebTransport adapter for h3zero
```

---

## h3zero Callback Flow

h3zero uses a path-based callback system. When a browser connects:

1. **CONNECT Request**: Browser sends HTTP/3 CONNECT to `/moq` endpoint
2. **Path Callback**: h3zero invokes `wtPathCallback(picohttp_callback_connect, ...)`
3. **Session Creation**: Server creates `PicoH3WebTransport` and returns 200 OK
4. **Subsequent Events**: All events for this session route to `handleWtEvent()`

```
Browser HTTP/3 CONNECT
    -> h3zero processes HTTP/3 frames
    -> wtPathCallback(picohttp_callback_connect, ...)
    -> MoQPicoH3EventBaseServer creates PicoH3WebTransport
    -> 200 OK sent to browser

Subsequent events:
    -> h3zero decodes HTTP/3 frames
    -> wtPathCallback(picohttp_callback_*, ...)
    -> PicoH3WebTransport::handleWtEvent(...)
```

### Event Dispatch Table

`handleWtEvent` dispatches based on `picohttp_callback_*` events:

| Event | Description | Handler |
|-------|-------------|---------|
| `picohttp_callback_post` | Initial POST/CONNECT body | Ignored (capsule data) |
| `picohttp_callback_post_data` | Stream data received | `onStreamData` |
| `picohttp_callback_post_fin` | Stream FIN received | `onStreamData(fin=true)` |
| `picohttp_callback_post_datagram` | Datagram received | `onReceiveDatagram` |
| `picohttp_callback_provide_data` | Ready to send stream data (JIT) | `onProvideData` |
| `picohttp_callback_provide_datagram` | Ready to send datagram | Dequeue and send |
| `picohttp_callback_reset` | Stream reset by peer | `onStreamReset` |
| `picohttp_callback_stop_sending` | Peer sent STOP_SENDING | `onStopSending` |
| `picohttp_callback_deregister` | Session closing | `onSessionClose` |

---

## Control Stream and Capsules

HTTP/3 WebTransport uses a control stream for session management:

- **Control Stream ID**: Stored in `controlStreamCtx_->stream_id`
- **Capsule Types**: `CLOSE_WEBTRANSPORT_SESSION`, `DRAIN_WEBTRANSPORT_SESSION`
- **Parsing**: `picowt_receive_capsule()` decodes capsules on the control stream

Data arriving on the control stream is parsed as WebTransport capsules, not MoQ data:

```cpp
if (streamId == controlStreamCtx_->stream_id) {
    picowt_receive_capsule(cnx_, controlStreamCtx_, bytes, bytes + length, &capsule);
    if (capsule.capsule_type == picowt_capsule_close_webtransport_session) {
        onSessionClose(capsule.error_code, nullptr);
    }
    return;  // Not passed to MoQSession
}
```

---

## Stream Creation (Egress)

Creating outgoing streams uses h3zero's WebTransport API:

```
MoQSession -> createUniStream()
  -> picowt_create_local_stream(cnx, is_bidir=0, h3Ctx, controlStreamId)
       // Returns h3zero_stream_ctx_t with WebTransport framing configured
  -> Copy path_callback from control stream for JIT routing
  -> streamContexts_[streamId] = streamCtx
  -> streamManager_->getOrCreateEgressHandle(streamId)
  -> markStreamActive(streamId)  // Trigger JIT callback
```

**Critical**: The `path_callback` must be copied from the control stream context
so h3zero routes `picohttp_callback_provide_data` to the correct handler.

---

## Stream Writes (JIT Model)

Like raw QUIC, HTTP/3 uses the JIT (Just-In-Time) send model:

### Step 1: Application queues data

```
MoQSession -> writeStreamData(id, IOBuf, fin)
  -> streamManager_->getOrCreateEgressHandle(id)->writeStreamData(data, fin)
  -> processEgressEvents()  // Marks streams active
  -> updateWakeTimeoutCallback_()  // Reschedule wake timer
```

### Step 2: h3zero calls back when ready

```
h3zero packet loop
  -> wtPathCallback(picohttp_callback_provide_data, streamCtx, context, maxLength)
  -> PicoH3WebTransport::onProvideData(streamCtx, context, maxLength)
       -> streamManager_->dequeue(*handle, maxLength)
       -> picoquic_provide_stream_data_buffer(context, dataLen, fin, isStillActive)
       -> memcpy IOBuf into returned buffer
       -> Fire delivery callback (optimistic)
       -> processEgressEvents()  // Activate other writable streams
```

### Sequence Diagram

```
┌─────────────┐     ┌──────────────────┐     ┌─────────────────┐     ┌─────────┐
│ MoQSession  │     │PicoH3WebTransport│     │ WtStreamManager │     │ h3zero  │
└──────┬──────┘     └────────┬─────────┘     └────────┬────────┘     └────┬────┘
       │                     │                        │                   │
       │ writeStreamData()   │                        │                   │
       │────────────────────>│                        │                   │
       │                     │ writeStreamData()      │                   │
       │                     │───────────────────────>│                   │
       │                     │                        │ (data buffered)   │
       │                     │<───────────────────────│                   │
       │                     │                        │                   │
       │                     │ processEgressEvents()  │                   │
       │                     │───────────────────────>│                   │
       │                     │ nextWritable()         │                   │
       │                     │<───────────────────────│                   │
       │                     │                        │                   │
       │                     │ markStreamActive()     │                   │
       │                     │───────────────────────────────────────────>│
       │                     │                        │                   │
       │                     │                        │    (later, when   │
       │                     │                        │     ready to send)│
       │                     │                        │                   │
       │                     │ picohttp_callback_provide_data             │
       │                     │<───────────────────────────────────────────│
       │                     │                        │                   │
       │                     │ onProvideData()        │                   │
       │                     │───────────────────────>│                   │
       │                     │ dequeue(maxLength)     │                   │
       │                     │<───────────────────────│                   │
       │                     │                        │                   │
       │                     │ picoquic_provide_stream_data_buffer()      │
       │                     │───────────────────────────────────────────>│
       │                     │                        │   (data on wire)  │
       │                     │                        │                   │
```

---

## Stream Reads (Ingress)

Incoming stream data flows through h3zero's callback:

```
h3zero receives HTTP/3 DATA frame
  -> wtPathCallback(picohttp_callback_post_data, streamCtx, bytes, length)
  -> PicoH3WebTransport::handleWtEvent(...)
  -> onStreamData(streamCtx, bytes, length, fin=false)
       -> streamContexts_[streamId] = streamCtx  // Track context
       -> streamManager_->getOrCreateIngressHandle(streamId)
       -> IOBuf::copyBuffer(bytes, length)
       -> streamManager_->enqueue(*handle, {data, fin})
       -> if streamId in pendingStreamNotifications_:
            -> handler_->onNewUniStream() or onNewBidiStream()
```

### Deferred Handler Notification

When a new peer stream arrives, `WtStreamManager::IngressCallback::onNewPeerStream`
is called during handle creation, but the handle isn't fully inserted yet (recursion
risk). The stream ID is stashed in `pendingStreamNotifications_` and the handler
is notified only after the first `enqueue` succeeds.

---

## Datagram Handling

### Sending Datagrams

```
MoQSession -> sendDatagram(IOBuf)
  -> datagramQueue_.push_back(datagram)
  -> h3zero_set_datagram_ready(cnx_, controlStreamId)

h3zero ready to send:
  -> wtPathCallback(picohttp_callback_provide_datagram, ...)
  -> h3zero_provide_datagram_buffer(context, dgLen, moreToSend)
  -> memcpy datagram into buffer
  -> datagramQueue_.pop_front()
```

### Receiving Datagrams

```
h3zero receives DATAGRAM frame
  -> wtPathCallback(picohttp_callback_post_datagram, bytes, length)
  -> onReceiveDatagram(bytes, length)
  -> handler_->onDatagram(IOBuf::copyBuffer(bytes, length))
```

---

## State Management

`PicoH3WebTransport` maintains several state structures:

| State | Type | Purpose |
|-------|------|---------|
| `cnx_` | `picoquic_cnx_t*` | QUIC connection handle |
| `h3Ctx_` | `h3zero_callback_ctx_t*` | h3zero HTTP/3 context |
| `controlStreamCtx_` | `h3zero_stream_ctx_t*` | WebTransport control stream |
| `streamContexts_` | `F14FastMap<uint64_t, h3zero_stream_ctx_t*>` | Stream ID → h3zero context |
| `datagramQueue_` | `deque<unique_ptr<IOBuf>>` | Pending outgoing datagrams |
| `pendingStreamNotifications_` | `F14FastSet<uint64_t>` | Peer streams awaiting notification |
| `streamManager_` | `unique_ptr<WtStreamManager>` | Read/write handles, buffering |
| `priorityQueue_` | `HTTPPriorityQueue` | Stream scheduling priority |

### Stream Context Lifecycle

1. **Creation**: `picowt_create_local_stream()` or first `onStreamData()` for peer streams
2. **Tracking**: Stored in `streamContexts_[streamId]`
3. **Cleanup**: Removed on `onStreamReset()` or session close

---

## Session Close

### Local Close

```
closeSession(error)
  -> sessionClosed_ = true
  -> streamManager_->shutdown(closeSession)
  -> picowt_send_close_session_message(cnx, controlStreamCtx, error, nullptr)
  -> handler_->onSessionEnd(error)
```

### Peer Close

```
h3zero receives CLOSE_WEBTRANSPORT_SESSION capsule on control stream
  -> onStreamData parses capsule via picowt_receive_capsule()
  -> onSessionClose(error)
       -> sessionClosed_ = true
       -> streamManager_->onCloseSession(closeSession)
       -> handler_->onSessionEnd(error)
```

### Close Sequence

```
┌─────────────┐     ┌──────────────────┐     ┌─────────────────┐     ┌─────────┐
│ MoQSession  │     │PicoH3WebTransport│     │ WtStreamManager │     │ h3zero  │
└──────┬──────┘     └────────┬─────────┘     └────────┬────────┘     └────┬────┘
       │                     │                        │                   │
       │ closeSession()      │                        │                   │
       │────────────────────>│                        │                   │
       │                     │ shutdown()             │                   │
       │                     │───────────────────────>│                   │
       │                     │                        │                   │
       │                     │ picowt_send_close_session_message()        │
       │                     │───────────────────────────────────────────>│
       │                     │                        │ (capsule on wire) │
       │                     │                        │                   │
       │ onSessionEnd()      │                        │                   │
       │<────────────────────│                        │                   │
       │                     │                        │                   │
```

---

## Key Differences from PicoQuicWebTransport

1. **Stream Context Tracking**: Must track `h3zero_stream_ctx_t*` per stream
   for JIT callbacks. Raw QUIC only needs the stream ID.

2. **Control Stream**: HTTP/3 WT has a dedicated control stream for capsules.
   Raw QUIC has no equivalent.

3. **Path Callback Propagation**: New streams must inherit `path_callback` from
   the control stream context, otherwise h3zero won't route JIT callbacks.

4. **Datagram API**: Uses `h3zero_set_datagram_ready` and `h3zero_provide_datagram_buffer`
   instead of `picoquic_mark_datagram_ready` and `picoquic_provide_datagram_buffer`.

5. **Flow Control**: h3zero handles HTTP/3 flow control internally. `WtStreamManager`
   limits are set to `max()` and `awaitUniStreamCredit`/`awaitBidiStreamCredit`
   return immediately.

---

## Error Handling

| Error Condition | Handling |
|-----------------|----------|
| Stream creation fails | Returns `ErrorCode::STREAM_CREATION_ERROR` |
| Invalid stream ID | Returns `ErrorCode::INVALID_STREAM_ID` |
| Session closed | Operations return error, no state changes |
| picoquic_provide_stream_data_buffer returns null | Logs error, data not sent |
| Capsule parse failure | Logs error, continues |

---

## Threading Model

Both `PicoQuicWebTransport` and `PicoH3WebTransport` are single-threaded:

**Threaded Model (MoQPicoQuicServer):**
- All callbacks execute on the packet loop thread
- `PicoQuicExecutor` runs coroutines on the same thread
- No cross-thread synchronization needed

**EventBase Model (MoQPicoQuicEventBaseServer):**
- All callbacks execute on the EventBase thread
- `MoQFollyExecutorImpl` schedules coroutines on the same EventBase
- `updateWakeTimeoutCallback_` must be called from EventBase thread

Common constraints:
- `WtStreamManager` operations are not thread-safe
- `handler_` callbacks execute synchronously
- Each `MoQSession` must only be accessed from its owning thread
