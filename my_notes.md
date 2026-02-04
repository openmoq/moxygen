# Moxygen Build Notes

## Build Matrix

| Configuration | MOXYGEN_USE_FOLLY | MOXYGEN_QUIC_BACKEND | Status | Tests | Notes |
|---------------|-------------------|----------------------|--------|-------|-------|
| Folly + mvfst | ON (default) | mvfst (default) | **Verified** | 250 pass | Full functionality, production ready |
| Std-mode + picoquic | OFF | picoquic | **Verified** | N/A | Callback-based API, no coroutines |
| Folly + picoquic | ON | picoquic | Partial | - | Needs getdeps manifest changes |

*Last verified: 2026-02-04*

### Hybrid: Std-mode Code with mvfst Transport

While you cannot build with `MOXYGEN_USE_FOLLY=OFF` and `MOXYGEN_QUIC_BACKEND=mvfst` (mvfst requires Folly),
the **MvfstStdModeAdapter** provides a bridge that allows callback-based (std-mode style) application code
to use mvfst transport:

| Scenario | Build Config | Application Code Style | Transport |
|----------|--------------|------------------------|-----------|
| Pure Folly | Folly + mvfst | Coroutines | mvfst |
| Pure std-mode | Std-mode + picoquic | Callbacks | picoquic |
| **Hybrid** | Folly + mvfst | Callbacks (via MvfstStdModeAdapter) | mvfst |

The MvfstStdModeAdapter:
- Is built as part of `Folly + mvfst` configuration (`libmoqtransport_mvfst_stdmode.a`)
- Provides `compat::WebTransportInterface` with callback-based API
- Internally manages a Folly EventBase thread
- Allows applications written without coroutines to still use mvfst transport

### Additional Options

| Option | Values | Default | Description |
|--------|--------|---------|-------------|
| MOXYGEN_TLS_BACKEND | openssl, mbedtls | openssl | TLS library for picoquic backend |

---

## Build Instructions

### Prerequisites

- CMake 3.16+
- C++20 compiler (clang 14+ or gcc 11+)
- Python 3 (for getdeps.py)

### 1. Folly + mvfst (Default - Full Featured)

This is the recommended configuration for production use. It includes:
- Full MoQ protocol support
- Coroutine-based async API
- mvfst QUIC transport (Meta's QUIC implementation)
- All samples, relay, and tests

```bash
# Clone the repository
git clone https://github.com/anthropics/moxygen.git
cd moxygen

# Build with getdeps (handles all dependencies)
./build/fbcode_builder/getdeps.py build \
  --src-dir=moxygen:. moxygen \
  --allow-system-packages \
  --scratch-path _build \
  --extra-cmake-defines '{"CMAKE_POLICY_VERSION_MINIMUM":"3.5"}'
```

**Build artifacts location:** `_build/installed/moxygen/`

**Libraries built:**
- `libmoxygen.a` - Core MoQ library
- `libmoqrelaysession.a` - Relay session support
- `libmoxygenserver.a` - Server library
- `libmoxygenclient.a` - Client library
- `libmoxygenwtclient.a` - WebTransport client
- `libmlogger.a` - MoQ logging
- `libmoqtransport_proxygen.a` - Proxygen transport adapter
- `libmoqtransport_mvfst_stdmode.a` - mvfst adapter for std-mode code

**Executables built:**
- `moqrelayserver` - MoQ relay server
- `moqdateserver` - Date sample server
- `moqtextclient` - Text client sample
- `moqtest_client` - Test client
- `moqtest_server` - Test server
- `moqflvstreamerclient` - FLV streamer
- `moqflvreceiverclient` - FLV receiver

---

### 2. Std-mode + picoquic (Minimal Dependencies)

This configuration uses standard C++ without Folly dependencies:
- Callback-based async API (no coroutines)
- picoquic QUIC transport
- Minimal external dependencies

```bash
# Build with std-mode and picoquic
./build/fbcode_builder/getdeps.py build \
  --src-dir=moxygen:. moxygen \
  --allow-system-packages \
  --scratch-path _build_std_pico \
  --extra-cmake-defines '{
    "CMAKE_POLICY_VERSION_MINIMUM":"3.5",
    "MOXYGEN_USE_FOLLY":"OFF",
    "MOXYGEN_QUIC_BACKEND":"picoquic"
  }'
```

**Libraries built:**
- `libmoxygen.a` - Core MoQ library (std-mode)
- `libmoqcompat.a` - Compatibility layer
- `libmoqtransport_picoquic.a` - picoquic transport adapter

**Notes:**
- No relay/samples/tests (require Folly)
- Uses `MoQSessionCompat` instead of `MoQSession`
- Callback-based API via `compat::ResultCallback`

---

### 3. Std-mode + picoquic with mbedTLS

For embedded systems or when OpenSSL is not available:

```bash
./build/fbcode_builder/getdeps.py build \
  --src-dir=moxygen:. moxygen \
  --allow-system-packages \
  --scratch-path _build_std_pico_mbedtls \
  --extra-cmake-defines '{
    "CMAKE_POLICY_VERSION_MINIMUM":"3.5",
    "MOXYGEN_USE_FOLLY":"OFF",
    "MOXYGEN_QUIC_BACKEND":"picoquic",
    "MOXYGEN_TLS_BACKEND":"mbedtls"
  }'
```

---

### 4. Folly + picoquic (Experimental)

Currently requires manual getdeps manifest changes to avoid building mvfst.

```bash
# Note: May fail due to dependency conflicts in getdeps manifest
./build/fbcode_builder/getdeps.py build \
  --src-dir=moxygen:. moxygen \
  --allow-system-packages \
  --scratch-path _build_folly_pico \
  --extra-cmake-defines '{
    "CMAKE_POLICY_VERSION_MINIMUM":"3.5",
    "MOXYGEN_QUIC_BACKEND":"picoquic"
  }'
```

**Status:** Partially working. The core library builds, but some components may have issues. Requires updates to the getdeps manifest to properly exclude mvfst dependencies.

---

## Running Tests

### Unit Tests (Folly + mvfst only)

```bash
# Run all tests
cd _build/build/moxygen
ctest --output-on-failure -j8

# Run specific test suite
ctest -R MoQSession --output-on-failure

# Run with verbose output
ctest -V -R MoQFramer
```

**Test categories:**
- `MoQFramer*` - Framing/parsing tests
- `MoQSession*` - Session management tests
- `MoQCache*` - Caching tests
- `MoQMi*` - Media interchange tests
- `MoQDeliveryTimer*` - Delivery timer tests

---

## Running Samples

### Date Server Example

The date server is a simple MoQ publisher that broadcasts the current time.

**Terminal 1 - Start the relay server:**
```bash
cd _build/installed/moxygen/bin

# Start relay on port 4433
./moqrelayserver \
  --port 4433 \
  --cert /path/to/cert.pem \
  --key /path/to/key.pem
```

**Terminal 2 - Start the date server (publisher):**
```bash
cd _build/installed/moxygen/bin

# Connect to relay and publish date/time
./moqdateserver \
  --connect_url "https://localhost:4433/moq" \
  --track_namespace "date" \
  --track_name "time"
```

**Terminal 3 - Start a text client (subscriber):**
```bash
cd _build/installed/moxygen/bin

# Subscribe to the date track
./moqtextclient \
  --connect_url "https://localhost:4433/moq" \
  --track_namespace "date" \
  --track_name "time"
```

### Generating Test Certificates

For local testing, generate self-signed certificates:

```bash
# Generate private key
openssl genrsa -out key.pem 2048

# Generate self-signed certificate
openssl req -new -x509 -key key.pem -out cert.pem -days 365 \
  -subj "/CN=localhost"
```

---

## Architecture Notes

### Transport Abstraction

The codebase supports multiple QUIC backends through `compat::WebTransportInterface`:

```
┌─────────────────────────────────────────────────────────────┐
│                    MoQSession / MoQSessionCompat            │
├─────────────────────────────────────────────────────────────┤
│                  compat::WebTransportInterface              │
├──────────────────────┬──────────────────────────────────────┤
│ ProxygenWebTransport │  PicoquicWebTransport                │
│   Adapter (mvfst)    │     Adapter                          │
├──────────────────────┼──────────────────────────────────────┤
│      mvfst QUIC      │       picoquic                       │
└──────────────────────┴──────────────────────────────────────┘
```

### MvfstStdModeAdapter (Hybrid Scenario)

The `MvfstStdModeAdapter` enables a powerful hybrid scenario: write application code using
simple callbacks (no coroutines required) while still leveraging mvfst's high-performance
QUIC implementation.

**Use case:** You want mvfst's performance but your application:
- Doesn't want to use Folly coroutines in application code
- Needs a simpler callback-based API
- Is being integrated with non-Folly event loops

**How to use:**

```cpp
#include <moxygen/transports/MvfstStdModeAdapter.h>
using namespace moxygen::transports;

// Create adapter with config
MvfstStdModeAdapter::Config config;
config.host = "localhost";
config.port = 4433;
config.verifyPeer = false;

// Optional: provide your own callback executor
// If not provided, callbacks run on mvfst's EventBase thread
config.callbackExecutor = [](std::function<void()> cb) {
    // Post to your event loop
    myEventLoop.post(std::move(cb));
};

auto adapter = MvfstStdModeAdapter::create(config);

// Connect asynchronously (non-blocking)
adapter->connect([adapter = adapter.get()](bool success) {
    if (!success) {
        std::cerr << "Connection failed" << std::endl;
        return;
    }

    // Create a unidirectional stream
    auto streamResult = adapter->createUniStream();
    if (streamResult.hasError()) {
        std::cerr << "Failed to create stream" << std::endl;
        return;
    }

    auto* writeHandle = streamResult.value();

    // Write data with callback
    auto payload = compat::Payload::copyBuffer("Hello, MoQ!");
    writeHandle->writeStreamData(std::move(payload), true,
        [](bool success) {
            std::cout << "Write " << (success ? "succeeded" : "failed") << std::endl;
        });
});

// Set up callbacks for incoming data
adapter->setNewUniStreamCallback([](compat::StreamReadHandle* readHandle) {
    readHandle->setReadCallback(
        [](compat::StreamData data, std::optional<uint32_t> error) {
            if (error) {
                std::cerr << "Read error: " << *error << std::endl;
                return;
            }
            if (data.data) {
                std::cout << "Received: " << data.data->toString() << std::endl;
            }
            if (data.fin) {
                std::cout << "Stream finished" << std::endl;
            }
        });
});

adapter->setDatagramCallback([](std::unique_ptr<compat::Payload> datagram) {
    std::cout << "Received datagram: " << datagram->toString() << std::endl;
});
```

**Linking:**

```cmake
# In your CMakeLists.txt
target_link_libraries(your_app
    moqtransport_mvfst_stdmode  # The adapter
    moxygen                      # Core MoQ
    # Folly and mvfst are pulled in transitively
)
```

**Key points:**
- Build moxygen with `Folly + mvfst` configuration (default)
- Link your application to `libmoqtransport_mvfst_stdmode.a`
- Use callback-based API in your application code (no coroutines needed)
- The adapter manages Folly EventBase thread internally
- Thread-safe: calls can be made from any thread

---

## Troubleshooting

### Build Failures

**"Cannot enable both MOXYGEN_QUIC_MVFST and MOXYGEN_QUIC_PICOQUIC"**
- Only one QUIC backend can be enabled at a time
- Check your CMake defines

**Linker errors for MLogger symbols**
- Ensure mlogger is linked (fixed in recent commits)
- Rebuild from clean

**Missing Folly/proxygen headers**
- Run getdeps.py to fetch dependencies
- Check `_build/installed/` for installed headers

### Runtime Issues

**Connection refused**
- Ensure server is running on the specified port
- Check firewall settings

**Certificate errors**
- Use `--insecure` flag for testing with self-signed certs
- Or provide proper certificate chain

**Protocol version mismatch**
- Both client and server must support compatible MoQ versions
- Check `MoQVersions.h` for supported versions

---

## File Locations

| Component | Path |
|-----------|------|
| Core library | `moxygen/` |
| Compatibility layer | `moxygen/compat/` |
| Transport adapters | `moxygen/transports/` |
| Relay implementation | `moxygen/relay/` |
| Samples | `moxygen/samples/` |
| Tests | `moxygen/test/` |
| Logging | `moxygen/mlog/` |
| Build config | `CMakeLists.txt`, `moxygen/CMakeLists.txt` |

---

## Version History

- **Current:** Supports MoQ draft versions (see `MoQVersions.h`)
- Transport abstraction allows mvfst or picoquic backends
- Std-mode support for environments without Folly
