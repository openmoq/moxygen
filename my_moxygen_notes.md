# My Moxygen Notes

## Table of Contents

- [Build Matrix](#build-matrix)
  - [Hybrid: Std-mode Code with mvfst Transport](#hybrid-std-mode-code-with-mvfst-transport)
  - [Additional CMake Options](#additional-cmake-options)
- [Quick Start by Build Mode](#quick-start-by-build-mode)
  - [Mode 1: Folly + mvfst (Full Featured)](#mode-1-folly--mvfst-full-featured)
  - [Mode 2: Std-mode + picoquic (Minimal Dependencies)](#mode-2-std-mode--picoquic-minimal-dependencies)
  - [Cross-Mode Interoperability](#cross-mode-interoperability)
- [Build](#build)
  - [Prerequisites](#prerequisites)
  - [Quick Build Workflow](#quick-build-workflow)
  - [CMake 4.x Workaround](#cmake-4x-workaround)
  - [Clean Build](#clean-build)
- [Build Configurations](#build-configurations)
  - [1. Folly + mvfst (Default)](#1-folly--mvfst-default---full-featured)
  - [2. Std-mode + picoquic](#2-std-mode--picoquic-minimal-dependencies)
  - [3. Std-mode + picoquic with mbedTLS](#3-std-mode--picoquic-with-mbedtls)
  - [4. Folly + picoquic](#4-folly--picoquic)
  - [5. Hybrid Mode (Callbacks + mvfst)](#5-hybrid-mode-callbacks--mvfst)
  - [Compat Layer Details](#compat-layer-details)
- [Running](#running)
  - [TLS / Certificates](#tls--certificates)
  - [Workflow 1: Date Server via Relay](#workflow-1-date-server-via-relay-pubsub)
  - [Workflow 2: Direct client-to-server](#workflow-2-direct-client-to-server-no-relay)
  - [Workflow 3: FLV streaming](#workflow-3-flv-streaming-through-relay)
  - [Workflow 4: Test suite](#workflow-4-test-suite)
  - [Workflow 5: Picoquic Date Server + Text Client](#workflow-5-picoquic-date-server--text-client-std-mode)
  - [Workflow 8: Advanced Std-mode Tests](#workflow-8-advanced-std-mode-tests-webtransport--interop)
  - [Debug Logging](#debug-logging)
- [Running Tests](#running-tests)
- [Key Flags Reference](#key-flags-reference)
- [Architecture Notes](#architecture-notes)
  - [Transport Abstraction](#transport-abstraction)
  - [Transport Mode Support (Cross-Flavor Interoperability)](#transport-mode-support-cross-flavor-interoperability)
  - [MvfstStdModeAdapter (Hybrid Scenario)](#mvfststdmodeadapter-hybrid-scenario)
- [Troubleshooting](#troubleshooting)
  - [Build Failures](#build-failures)
  - [Runtime Issues](#runtime-issues)
- [File Locations](#file-locations)
- [Build Output Details](#build-output-details)
  - [Moxygen Libraries](#moxygen-libraries-folly--mvfst-build)
  - [Object Files in Key Libraries](#object-files-in-key-libraries)
  - [Executables](#executables)
  - [Dynamic Library Dependencies (macOS)](#dynamic-library-dependencies-macos)
  - [Key Symbols in Hybrid Adapter](#key-symbols-in-hybrid-adapter)
- [Dependency Notes](#dependency-notes)
  - [Why Folly/Proxygen Are Built Even With MOXYGEN_USE_FOLLY=OFF](#why-follproxygen-are-built-even-with-moxygen_use_follyoff)
  - [Dependency Source Summary](#dependency-source-summary)
- [Version History](#version-history)

---

## Build Matrix

| Configuration | MOXYGEN_USE_FOLLY | MOXYGEN_QUIC_BACKEND | Status | Tests | Notes |
|---------------|-------------------|----------------------|--------|-------|-------|
| Folly + mvfst | ON (default) | mvfst (default) | **Verified** | 250 pass | Full functionality, production ready |
| Std-mode + picoquic | OFF | picoquic | **Verified** | 64 unit + 11 integration | Callback-based API, end-to-end data delivery works |
| Folly + picoquic | ON | picoquic | **Verified** | Uses std-mode tests | Callback-based API with Folly utilities |

*Last verified: 2026-02-09*

### Hybrid: Std-mode Code with mvfst Transport

While you cannot build with `MOXYGEN_USE_FOLLY=OFF` and `MOXYGEN_QUIC_BACKEND=mvfst` (mvfst requires Folly),
the **MvfstStdModeAdapter** provides a bridge that allows callback-based (std-mode style) application code
to use mvfst transport:

| Scenario | Build Config | Application Code Style | Transport |
|----------|--------------|------------------------|-----------|
| Pure Folly | Folly + mvfst | Coroutines | mvfst |
| Pure std-mode | Std-mode + picoquic | Callbacks | picoquic |
| **Hybrid** | Folly + mvfst | Callbacks (via MvfstStdModeAdapter) | mvfst |

**Why use Hybrid mode?**
- You want mvfst's high-performance QUIC implementation
- Your application code doesn't want to use Folly coroutines
- You're integrating with a non-Folly event loop (libuv, libevent, etc.)
- You want a simpler callback-based API

### Additional CMake Options

| Option | Values | Default | Description |
|--------|--------|---------|-------------|
| MOXYGEN_TLS_BACKEND | openssl, mbedtls | openssl | TLS library for picoquic backend |

---

## Quick Start by Build Mode

This section provides copy-paste commands for each build mode. Each subsection is self-contained
with clean, build, and run commands for both raw QUIC and WebTransport.

---

### Mode 1: Folly + mvfst (Full Featured)

**When to use:** Production deployments, full MoQ functionality, coroutine-based API.

#### Clean

```bash
# Full clean (removes all build artifacts and dependencies)
rm -rf _build

# Partial clean (just moxygen, keeps dependencies - faster rebuild)
rm -rf _build/build/moxygen _build/installed/moxygen
```

#### Build

```bash
# Install system dependencies (once per machine)
brew install zlib openssl@3 libsodium lz4 xz zstd icu4c fmt  # macOS

# Build (~30 min first time, ~2 min incremental)
./build/fbcode_builder/getdeps.py build --src-dir=moxygen:. moxygen \
  --allow-system-packages \
  --scratch-path _build \
  --extra-cmake-defines '{"CMAKE_POLICY_VERSION_MINIMUM":"3.5"}'

# Fix dylib paths (required on macOS after each build)
# NOTE: Must use $(pwd) to create absolute symlinks!
mkdir -p _build/deps/lib
find "$(pwd)/_build/installed" -name "*.dylib" -exec ln -sf {} _build/deps/lib/ \;

# Add binaries to PATH
export PATH="$(pwd)/_build/installed/moxygen/bin:$PATH"
```

#### Run: Raw QUIC Transport

```bash
# Terminal 1: Start relay server
moqrelayserver --insecure --port 4433

# Terminal 2: Start date publisher (connects to relay with raw QUIC)
moqdateserver --insecure --port 4434 --quic_transport \
  --relay_url "https://localhost:4433/moq-relay" \
  --ns "moq-date"

# Terminal 3: Subscribe via raw QUIC
moqtextclient --insecure --quic_transport \
  --connect_url "https://localhost:4433/moq-relay" \
  --track_namespace "moq-date" \
  --track_name "date"
```

#### Run: WebTransport (HTTP/3)

```bash
# Terminal 1: Start relay server
moqrelayserver --insecure --port 4433

# Terminal 2: Start date publisher (connects via WebTransport - default)
moqdateserver --insecure --port 4434 \
  --relay_url "https://localhost:4433/moq-relay" \
  --ns "moq-date"

# Terminal 3: Subscribe via WebTransport (default)
moqtextclient --insecure \
  --connect_url "https://localhost:4433/moq-relay" \
  --track_namespace "moq-date" \
  --track_name "date"
```

#### Run: Direct Connection (No Relay)

```bash
# Terminal 1: Start date server standalone
moqdateserver --insecure --port 4433 --ns "moq-date"

# Terminal 2: Connect directly (note: path is /moq-date, not /moq-relay)
moqtextclient --insecure \
  --connect_url "https://localhost:4433/moq-date" \
  --track_namespace "moq-date" \
  --track_name "date"
```

#### Key Binaries

| Binary | Description |
|--------|-------------|
| `moqrelayserver` | MoQ relay/broker server |
| `moqdateserver` | Sample date publisher |
| `moqtextclient` | Sample text subscriber |
| `moqflvstreamerclient` | FLV file publisher |
| `moqflvreceiverclient` | FLV file receiver |

---

### Mode 2: Std-mode + picoquic (Minimal Dependencies)

**When to use:** Embedded systems, no Folly dependency, callback-based API, fast builds.

#### Clean

```bash
# Full clean
rm -rf _build_std

# Partial clean (keep picoquic dependency)
rm -rf _build_std/moxygen
```

#### Build

```bash
# Prerequisites (only OpenSSL needed!)
brew install openssl@3  # macOS
# apt install libssl-dev  # Linux

# Build (~2 min, no getdeps needed!)
mkdir -p _build_std && cd _build_std
cmake .. \
  -DMOXYGEN_USE_FOLLY=OFF \
  -DMOXYGEN_QUIC_BACKEND=picoquic \
  -DMOXYGEN_TLS_BACKEND=openssl \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build . --parallel

# Binaries are in:
#   _build_std/moxygen/samples/date/picodateserver
#   _build_std/moxygen/samples/text-client/picotextclient
```

#### Generate TLS Certificates (Required)

```bash
# Generate self-signed cert for testing
openssl genrsa -out key.pem 2048
openssl req -new -x509 -key key.pem -out cert.pem -days 365 -subj "/CN=localhost"
```

#### Run: Raw QUIC Transport (Default)

```bash
# Terminal 1: Start picoquic date server (raw QUIC is default)
./moxygen/samples/date/picodateserver \
  --cert cert.pem --key key.pem \
  --port 4433 --ns moq-date --mode spg

# Terminal 2: Connect picoquic text client (raw QUIC is default)
./moxygen/samples/text-client/picotextclient \
  --host localhost --port 4433 \
  --ns moq-date --track date --insecure
```

#### Run: WebTransport (HTTP/3)

```bash
# Terminal 1: Start server with WebTransport
./moxygen/samples/date/picodateserver \
  --cert cert.pem --key key.pem \
  --port 4433 --ns moq-date --mode spg \
  --transport webtransport

# Terminal 2: Connect client with WebTransport
./moxygen/samples/text-client/picotextclient \
  --host localhost --port 4433 \
  --ns moq-date --track date --insecure \
  --transport webtransport
```

#### Key Binaries

| Binary | Description |
|--------|-------------|
| `picodateserver` | Date publisher (picoquic transport) |
| `picotextclient` | Text subscriber (picoquic transport) |

#### Command-Line Flags

**picodateserver:**
| Flag | Default | Description |
|------|---------|-------------|
| `--cert, -c` | (required) | TLS certificate file |
| `--key, -k` | (required) | TLS private key file |
| `--port, -p` | 4433 | Server port |
| `--ns, -n` | moq-date | Track namespace |
| `--mode, -m` | spg | Transmit mode: spg, spo, datagram |
| `--transport, -t` | quic | Transport: `quic` or `webtransport` |

**picotextclient:**
| Flag | Default | Description |
|------|---------|-------------|
| `--host, -H` | (required) | Server hostname |
| `--port, -p` | 4433 | Server port |
| `--ns, -n` | moq-date | Track namespace |
| `--track` | date | Track name |
| `--transport, -T` | quic | Transport: `quic` or `webtransport` |
| `--insecure, -k` | false | Skip TLS cert validation |

---

### Cross-Mode Interoperability

Clients and servers from different build modes can communicate when using WebTransport
(both use `h3` ALPN for HTTP/3).

#### Picoquic Client → mvfst Relay

```bash
# Terminal 1: Start mvfst relay (Folly + mvfst build)
moqrelayserver --insecure --port 4433

# Terminal 2: Start mvfst date publisher
moqdateserver --insecure --port 4434 \
  --relay_url "https://localhost:4433/moq-relay" \
  --ns "moq-date"

# Terminal 3: Connect picoquic client via WebTransport
./moxygen/samples/text-client/picotextclient \
  --host localhost --port 4433 \
  --ns moq-date --track date --insecure \
  --transport webtransport
```

#### mvfst Client → Picoquic Server

```bash
# Terminal 1: Start picoquic server with WebTransport
./moxygen/samples/date/picodateserver \
  --cert cert.pem --key key.pem \
  --port 4433 --ns moq-date \
  --transport webtransport

# Terminal 2: Connect mvfst client
moqtextclient --insecure \
  --connect_url "https://localhost:4433/moq-date" \
  --track_namespace "moq-date" \
  --track_name "date"
```

#### Transport Mode Summary

| Mode | ALPN | Flag (picoquic) | Flag (mvfst) |
|------|------|-----------------|--------------|
| Raw QUIC | `moq-00` | `--transport quic` (default) | `--quic_transport` |
| WebTransport | `h3` | `--transport webtransport` | (default, no flag) |

**Note:** Raw QUIC mode (`moq-00` ALPN) is NOT interoperable between picoquic and mvfst builds.
Use WebTransport mode for cross-build communication.

---

## Build

Moxygen uses CMake and Meta's `getdeps.py` build orchestrator (`build/fbcode_builder/getdeps.py`).

### Prerequisites

- CMake 3.16+
- C++20 compiler (clang 14+ or gcc 11+)
- Python 3 (for getdeps.py)

**macOS (Homebrew):** Install system dependencies first. This is critical because
getdeps downloads some dependencies from source (e.g. zlib from zlib.net), and
those upstream URLs break regularly when new versions are released. Using
`--allow-system-packages` with homebrew-installed packages avoids these failures.

```bash
brew install zlib openssl@3 libsodium lz4 xz zstd icu4c fmt
```

### Quick Build Workflow

```bash
# 1. Install OS-level dependencies (run once per machine)
./build/fbcode_builder/getdeps.py install-system-deps --recursive moxygen

# 2. Build using local source and system packages
#    --allow-system-packages is IMPORTANT: skips downloading deps that are
#    already installed via homebrew/apt, avoiding broken upstream URLs
./build/fbcode_builder/getdeps.py build --src-dir=moxygen:. moxygen \
  --allow-system-packages \
  --scratch-path _build \
  --extra-cmake-defines '{"CMAKE_POLICY_VERSION_MINIMUM":"3.5"}'
```

### What Each Step Does

**`install-system-deps moxygen`** - Walks the full dependency tree and installs
required OS packages via the system package manager (apt, dnf, brew, pacman).
Run once per environment. Use `--recursive` to include transitive deps.

**`--src-dir=moxygen:.`** - Tells getdeps to use the current directory as the
moxygen source instead of fetching it from git.

**`--allow-system-packages`** - Lets getdeps skip building dependencies that are
already satisfied by installed system packages. Much faster than building
everything from source.

**`--scratch-path _build`** - Controls where checkouts, build artifacts, and installed
deps are stored:
- `_build/build/` - build artifacts
- `_build/installed/` - installed dependencies
- `_build/downloads/` - downloaded source archives
- `_build/extracted/` - extracted source code

### CMake 4.x Workaround

CMake 4.x removed compatibility with `cmake_minimum_required` < 3.5, which
breaks some older dependencies (e.g. `double-conversion`). Add this to all
build commands:

```bash
--extra-cmake-defines '{"CMAKE_POLICY_VERSION_MINIMUM":"3.5"}'
```

### Clean Build

To completely clean and rebuild:

```bash
# Remove all build artifacts for a specific scratch path
rm -rf _build

# Or selectively clean:
rm -rf _build/build/moxygen      # Just moxygen build artifacts
rm -rf _build/installed/moxygen  # Just moxygen installed files

# Clean and rebuild from scratch
rm -rf _build && ./build/fbcode_builder/getdeps.py build \
  --src-dir=moxygen:. moxygen \
  --allow-system-packages \
  --scratch-path _build \
  --extra-cmake-defines '{"CMAKE_POLICY_VERSION_MINIMUM":"3.5"}'
```

**What each directory contains:**
```
_build/
├── build/           # CMake build trees (object files, CMakeCache.txt)
│   ├── moxygen/     # Moxygen build (~500MB)
│   ├── folly/       # Folly build (~1GB)
│   └── ...
├── installed/       # Installed artifacts (headers, libs, bins)
│   ├── moxygen/     # Moxygen install (~100MB)
│   │   ├── bin/     # Executables
│   │   └── lib/     # Static libraries
│   └── ...
├── downloads/       # Downloaded tarballs
└── extracted/       # Extracted source code
```

**Partial rebuilds:**
```bash
# Rebuild just moxygen (keeps dependencies)
rm -rf _build/build/moxygen _build/installed/moxygen
./build/fbcode_builder/getdeps.py build --src-dir=moxygen:. moxygen \
  --allow-system-packages --scratch-path _build \
  --extra-cmake-defines '{"CMAKE_POLICY_VERSION_MINIMUM":"3.5"}'
```

---

## Build Configurations

### 1. Folly + mvfst (Default - Full Featured)

This is the recommended configuration for production use. It includes:
- Full MoQ protocol support
- Coroutine-based async API
- mvfst QUIC transport (Meta's QUIC implementation)
- All samples, relay, and tests

```bash
./build/fbcode_builder/getdeps.py build --src-dir=moxygen:. moxygen \
  --allow-system-packages \
  --scratch-path _build \
  --extra-cmake-defines '{"CMAKE_POLICY_VERSION_MINIMUM":"3.5"}'
```

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

**Dependency chain (24 unique packages on macOS):**

```
moxygen
├── zlib
├── gperf
├── folly
│   ├── gflags
│   ├── glog
│   │   └── gflags
│   ├── boost
│   ├── libdwarf
│   ├── libevent
│   ├── libsodium
│   ├── double-conversion
│   ├── fast_float
│   ├── fmt
│   ├── lz4
│   ├── snappy
│   ├── zstd
│   ├── openssl          (macOS)
│   ├── xz               (not Windows)
│   ├── libunwind         (Linux only)
│   ├── libaio            (Linux only)
│   └── libiberty         (Linux only)
├── fizz
│   ├── folly             (already listed)
│   ├── liboqs
│   ├── libsodium         (already listed)
│   ├── zlib              (already listed)
│   └── zstd              (already listed)
├── wangle
│   ├── folly             (already listed)
│   └── fizz              (already listed)
├── mvfst
│   ├── folly             (already listed)
│   └── fizz              (already listed)
└── proxygen
    ├── folly             (already listed)
    ├── fizz              (already listed)
    ├── wangle            (already listed)
    ├── mvfst             (already listed)
    ├── c-ares
    ├── zlib              (already listed)
    └── gperf             (already listed)
```

---

### 2. Std-mode + picoquic (Minimal Dependencies)

This configuration uses standard C++ without Folly dependencies:
- Callback-based async API (no coroutines)
- picoquic QUIC transport
- Minimal external dependencies

#### Option A: Direct CMake Build (Recommended - No getdeps!)

For std-mode + picoquic, **you don't need getdeps at all**. The only real dependencies
are OpenSSL (system) and picoquic (auto-fetched by CMake via FetchContent). This skips
building the 24 unused packages (folly, proxygen, mvfst, etc.) that getdeps would build.

**Prerequisites:** `brew install openssl@3` (macOS) or `apt install libssl-dev` (Linux)

```bash
# Direct CMake build (no getdeps, no Folly, ~2 min vs ~30 min)
mkdir -p _build_std && cd _build_std
cmake .. \
  -DMOXYGEN_USE_FOLLY=OFF \
  -DMOXYGEN_QUIC_BACKEND=picoquic \
  -DMOXYGEN_TLS_BACKEND=openssl \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build . --parallel
```

**Binaries produced:**
```
moxygen/samples/date/picodateserver          # ~1.2MB
moxygen/samples/text-client/picotextclient   # ~1.3MB
```

#### Option B: Via getdeps (builds unused dependencies)

```bash
./build/fbcode_builder/getdeps.py build --src-dir=moxygen:. moxygen \
  --allow-system-packages \
  --scratch-path _build_std_pico \
  --extra-cmake-defines '{
    "CMAKE_POLICY_VERSION_MINIMUM":"3.5",
    "MOXYGEN_USE_FOLLY":"OFF",
    "MOXYGEN_QUIC_BACKEND":"picoquic"
  }'
```

**Libraries built:**
- `libmoxygen.a` - Core MoQ library (std-mode, includes MoQSessionCompat)
- `libmoqcompat.a` - Compatibility layer
- `libmoqtransport_picoquic.a` - picoquic transport adapter
- `libmoqsimpleexecutor.a` - Simple task executor (std-mode)

**Executables built:**
- `picodateserver` - Date server using picoquic transport
- `picotextclient` - Text client using picoquic transport

**Running the examples:**

```bash
# Generate TLS certs (required for QUIC)
openssl genrsa -out key.pem 2048
openssl req -new -x509 -key key.pem -out cert.pem -days 365 -subj "/CN=localhost"

# Terminal 1: Start picoquic date server
./moxygen/samples/date/picodateserver --cert cert.pem --key key.pem --port 4433 --ns moq-date --mode spg

# Terminal 2: Connect picoquic text client
./moxygen/samples/text-client/picotextclient --host localhost --port 4433 --ns moq-date --track date --insecure
```

See [Workflow 5](#workflow-5-picoquic-date-server--text-client-std-mode) for full protocol flow
and flag reference.

**Notes:**
- Uses `MoQSessionCompat` (callback-based) instead of `MoQSession` (coroutine-based)
- `MoQSessionCompat` is fully implemented: SETUP handshake, subscribe, fetch, data
  delivery (subgroups, object streams, datagrams), subscribe done, unsubscribe, and
  all control message handlers
- Callback-based API via `compat::ResultCallback` (no coroutines)
- `MoQSimpleExecutor` provides a simple single-threaded event loop (replaces Folly's EventBase)
- Key internal classes: `CompatTrackPublisher` (server publish path),
  `CompatStreamPublisher` (writes objects to streams), `CompatObjectStreamCallback`
  (receives objects from uni streams), `CompatReceiverSubscriptionHandle` (client
  subscribe handle)

**Dependency chain (3 packages, no getdeps needed):**

```
moxygen (MOXYGEN_USE_FOLLY=OFF, MOXYGEN_QUIC_BACKEND=picoquic)
├── picoquic          (fetched via CMake FetchContent, NOT getdeps)
│   ├── picotls
│   │   └── openssl   (or mbedtls if MOXYGEN_TLS_BACKEND=mbedtls)
│   └── openssl       (or mbedtls)
└── openssl           (or mbedtls)
```

picoquic is NOT in the fbcode_builder manifests. It is fetched at CMake configure
time via `FetchContent_Declare` in `moxygen/transports/CMakeLists.txt`.

If you use getdeps (Option B), it will build all 24 packages from Flavor 1 because
the manifest doesn't change, but only picoquic + openssl/mbedtls are linked into
the final binaries.

---

### 3. Std-mode + picoquic with mbedTLS

For embedded systems or when OpenSSL is not available:

```bash
./build/fbcode_builder/getdeps.py build --src-dir=moxygen:. moxygen \
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

### 4. Folly + picoquic

Uses Folly utilities with picoquic QUIC transport:

```bash
./build/fbcode_builder/getdeps.py build --src-dir=moxygen:. moxygen \
  --allow-system-packages \
  --scratch-path _build_folly_pico \
  --extra-cmake-defines '{
    "CMAKE_POLICY_VERSION_MINIMUM":"3.5",
    "MOXYGEN_QUIC_BACKEND":"picoquic"
  }'
```

**Libraries built:**
- `libmoxygen.a` - Core MoQ library
- `libmlogger.a` - MoQ logging
- `libmoqtransport_picoquic.a` - picoquic transport adapter

**Dependency chain:**

```
moxygen (MOXYGEN_USE_FOLLY=ON, MOXYGEN_QUIC_BACKEND=picoquic)
├── folly (full chain: ~20 packages, same as Flavor 1)
├── picoquic          (via CMake FetchContent)
│   ├── picotls
│   │   └── openssl
│   └── openssl
├── zlib
└── gperf
```

fizz, wangle, mvfst, and proxygen are built by getdeps but NOT linked
(picoquic replaces mvfst, no proxygen needed). Folly IS linked for utilities.

---

### 5. Hybrid Mode (Callbacks + mvfst)

Hybrid mode uses the default Folly + mvfst build but your application uses callback-based APIs
via `MvfstStdModeAdapter` instead of coroutines.

**Build (same as default):**
```bash
./build/fbcode_builder/getdeps.py build --src-dir=moxygen:. moxygen \
  --allow-system-packages \
  --scratch-path _build \
  --extra-cmake-defines '{"CMAKE_POLICY_VERSION_MINIMUM":"3.5"}'
```

**Key library for hybrid mode:**
- `libmoqtransport_mvfst_stdmode.a` - The adapter that bridges callback API to mvfst

**Your CMakeLists.txt:**
```cmake
find_package(moxygen REQUIRED)

add_executable(my_hybrid_app main.cpp)
target_link_libraries(my_hybrid_app
    moqtransport_mvfst_stdmode  # Callback-to-mvfst adapter
    moxygen                      # Core MoQ library
    # Folly/mvfst are linked transitively
)
```

**Minimal test example (my_hybrid_test.cpp):**
```cpp
#include <moxygen/transports/MvfstStdModeAdapter.h>
#include <moxygen/MoQSession.h>
#include <iostream>
#include <thread>
#include <chrono>

using namespace moxygen;
using namespace moxygen::transports;

int main() {
    // 1. Create adapter config
    MvfstStdModeAdapter::Config config;
    config.host = "localhost";
    config.port = 4433;
    config.verifyPeer = false;  // For testing with self-signed certs

    // 2. Create the adapter
    auto adapter = MvfstStdModeAdapter::create(config);
    if (!adapter) {
        std::cerr << "Failed to create adapter" << std::endl;
        return 1;
    }

    // 3. Set up callbacks for incoming streams/datagrams
    adapter->setNewUniStreamCallback([](compat::StreamReadHandle* stream) {
        std::cout << "New uni stream: " << stream->getID() << std::endl;
        stream->setReadCallback(
            [](compat::StreamData data, std::optional<uint32_t> error) {
                if (error) {
                    std::cerr << "Stream error: " << *error << std::endl;
                    return;
                }
                if (data.data) {
                    std::cout << "Received data: " << data.data->computeChainDataLength()
                              << " bytes" << std::endl;
                }
            });
    });

    adapter->setSessionCloseCallback([](std::optional<uint32_t> error) {
        if (error) {
            std::cerr << "Session closed with error: " << *error << std::endl;
        } else {
            std::cout << "Session closed cleanly" << std::endl;
        }
    });

    // 4. Connect asynchronously
    std::atomic<bool> connected{false};
    std::atomic<bool> done{false};

    adapter->connect([&](bool success) {
        if (!success) {
            std::cerr << "Connection failed" << std::endl;
            done = true;
            return;
        }
        std::cout << "Connected to " << config.host << ":" << config.port << std::endl;
        connected = true;

        // 5. Create a stream and send data
        auto streamResult = adapter->createUniStream();
        if (streamResult.hasError()) {
            std::cerr << "Failed to create stream" << std::endl;
            done = true;
            return;
        }

        auto* writeHandle = streamResult.value();
        auto payload = compat::Payload::copyBuffer("Hello from hybrid mode!");
        writeHandle->writeStreamData(std::move(payload), true,
            [&](bool writeSuccess) {
                std::cout << "Write " << (writeSuccess ? "succeeded" : "failed")
                          << std::endl;
                done = true;
            });
    });

    // Wait for completion (in real app, use your event loop)
    while (!done) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // 6. Cleanup
    adapter->closeSession();
    return 0;
}
```

**Testing hybrid mode end-to-end:**

```bash
# Terminal 1: Start the relay server (uses coroutines internally)
_build/installed/moxygen/bin/moqrelayserver --insecure --port 4433

# Terminal 2: Run your hybrid app (uses callbacks)
./my_hybrid_app
```

**Server-side hybrid mode (MvfstStdModeServer):**

For servers that want callback-based handling:

```cpp
#include <moxygen/transports/MvfstStdModeAdapter.h>

using namespace moxygen::transports;

int main() {
    MvfstStdModeServer::Config config;
    config.host = "0.0.0.0";
    config.port = 4433;
    config.certPath = "cert.pem";
    config.keyPath = "key.pem";

    config.onNewSession = [](std::unique_ptr<compat::WebTransportInterface> session) {
        std::cout << "New session from: "
                  << session->getPeerAddress().getAddressStr() << std::endl;

        // Handle the session with callbacks
        session->setNewUniStreamCallback([](compat::StreamReadHandle* stream) {
            // Handle incoming streams...
        });
    };

    auto server = MvfstStdModeServer::create(config);
    server->start([](bool success) {
        if (success) {
            std::cout << "Server started" << std::endl;
        }
    });

    // Run until interrupted
    std::this_thread::sleep_for(std::chrono::hours(24));
    return 0;
}
```

---

### Compat Layer Details

The `moxygen::compat` abstraction layer allows building with or without Folly:

**Folly mode (default):**
- Uses `folly::Expected`, `folly::coro::Task`, `folly::SemiFuture`, `folly::IOBuf`
- Full functionality with coroutines and efficient buffer management
- Requires Folly, Proxygen, and their dependencies

**Std mode (`MOXYGEN_USE_FOLLY=OFF`):**
- Uses std-compatible alternatives via `moxygen::compat` types
- `compat::Expected<T,E>` - std::variant-based Expected
- `compat::Task<T>` - callback-based async (no coroutines)
- `compat::Payload` - std::vector-based buffer abstraction
- Minimal dependencies, suitable for embedded/mobile platforms

The compat types are defined in `moxygen/compat/`:
- `Config.h` - `MOXYGEN_USE_FOLLY` detection
- `Expected.h` - `compat::Expected<T,E>`, `compat::makeUnexpected()`
- `Unit.h` - `compat::Unit`, `compat::unit`
- `Async.h` - `compat::Task<T>`, `compat::SemiFuture<T>`
- `Payload.h` - `compat::Payload` interface
- `IOBufPayload.h` - Folly IOBuf wrapper (folly mode only)

---

## Running

Binaries are installed to `_build/installed/moxygen/bin/`. For convenience:

```bash
export PATH="$(pwd)/_build/installed/moxygen/bin:$PATH"
```

On macOS, shared libraries are installed in hash-named directories but binaries have
`@rpath` set to `_build/deps/lib`. Symlink all dylibs there:

```bash
mkdir -p _build/deps/lib
find "$(pwd)/_build/installed" -name "*.dylib" -exec ln -sf {} _build/deps/lib/ \;
```

### TLS / Certificates

All servers need TLS. For development, use `--insecure` on both server and
clients to skip certificate validation. For production, use proper certs:

```bash
--cert ./certs/certificate.pem --key ./certs/certificate.key
```

Generate self-signed certificates for testing:

```bash
# Generate private key
openssl genrsa -out key.pem 2048

# Generate self-signed certificate
openssl req -new -x509 -key key.pem -out cert.pem -days 365 \
  -subj "/CN=localhost"
```

---

### Workflow 1: Date Server via Relay (pub/sub)

3 terminals: relay + publisher + subscriber. The relay acts as a broker.

**Common mistakes:**
- Date server and relay must be on **different ports** (`--port 4434` vs `--port 4433`)
- Date server must use `--relay_url` to connect TO the relay as a publisher
- Without `--relay_url`, the date server runs standalone and doesn't register with the relay
- Client's `--connect_url` path must be `/moq-relay` (the relay endpoint, not the namespace)

```
  moqdateserver                moqrelayserver              moqtextclient
  (publisher)                     (relay)                  (subscriber)
      |                              |                          |
      |--- QUIC connect :4433 ------>|                          |
      |--- CLIENT_SETUP ------------>|                          |
      |<-- SERVER_SETUP -------------|                          |
      |--- PUBLISH_NAMESPACE ------->|                          |
      |<-- PUBLISH_NAMESPACE_OK -----|                          |
      |                              |<--- QUIC connect :4433 --|
      |                              |<--- CLIENT_SETUP --------|
      |                              |---- SERVER_SETUP ------->|
      |                              |<--- SUBSCRIBE -----------|
      |<-- SUBSCRIBE ----------------|                          |
      |--- SUBSCRIBE_OK ------------>|---- SUBSCRIBE_OK ------->|
      |                              |                          |
      |--- OBJECT (group/subgroup) ->|---- OBJECT ------------>| "2026-02-04 14:39:00"
      |--- OBJECT ------------------->|---- OBJECT ------------>| "2026-02-04 14:39:01"
      :                              :                          :
```

```bash
# Terminal 1: Start relay
moqrelayserver --insecure --port 4433

# Terminal 2: Start date publisher (connects to relay)
moqdateserver --insecure --port 4434 \
  --relay_url "https://localhost:4433/moq-relay" \
  --ns "moq-date"

# Terminal 3: Subscribe to date track through relay
moqtextclient --insecure \
  --connect_url "https://localhost:4433/moq-relay" \
  --track_namespace "moq-date" \
  --track_name "date"
```

**With raw QUIC transport** (instead of WebTransport):
```bash
# Terminal 1: Relay (no change needed, accepts both)
moqrelayserver --insecure --port 4433

# Terminal 2: Date server with raw QUIC
moqdateserver --insecure --port 4434 --quic_transport \
  --relay_url "https://localhost:4433/moq-relay" \
  --ns "moq-date"

# Terminal 3: Client with raw QUIC
moqtextclient --insecure --quic_transport \
  --connect_url "https://localhost:4433/moq-relay" \
  --track_namespace "moq-date" \
  --track_name "date"
```

### Workflow 2: Direct client-to-server (no relay)

2 terminals only. The client connects directly to the date server.
**Important:** The `--connect_url` path must match the server's namespace, NOT `/moq-relay`.

```
  moqdateserver                              moqtextclient
  (server+publisher)                          (subscriber)
      |                                            |
      |  listening on :4433                        |
      |<------------ QUIC connect :4433 -----------|
      |<------------ CLIENT_SETUP -----------------|
      |------------- SERVER_SETUP ---------------->|
      |<------------ SUBSCRIBE --------------------|
      |------------- SUBSCRIBE_OK ---------------->|
      |                                            |
      |------------- OBJECT (date/time) ---------->| "2026-02-04 14:39:00"
      |------------- OBJECT ---------------------->| "2026-02-04 14:39:01"
      :                                            :
```

```bash
# Terminal 1: Start date server standalone
moqdateserver --insecure --port 4433 --ns "moq-date"

# Terminal 2: Connect directly to date server (note: path is /moq-date, not /moq-relay)
moqtextclient --insecure \
  --connect_url "https://localhost:4433/moq-date" \
  --track_namespace "moq-date" \
  --track_name "date"
```

**With raw QUIC transport:**
```bash
# Terminal 1
moqdateserver --insecure --port 4433 --quic_transport --ns "moq-date"

# Terminal 2
moqtextclient --insecure --quic_transport \
  --connect_url "https://localhost:4433/moq-date" \
  --track_namespace "moq-date" \
  --track_name "date"
```

### Workflow 3: FLV streaming through relay

```
  moqflvstreamerclient        moqrelayserver          moqflvreceiverclient
  (reads input.flv)              (relay)              (writes output.flv)
      |                              |                          |
      |--- QUIC connect ----------->|                          |
      |--- CLIENT/SERVER_SETUP ---->|                          |
      |--- PUBLISH_NAMESPACE ------>|                          |
      |                              |<--- QUIC connect --------|
      |                              |<--- CLIENT/SERVER_SETUP -|
      |                              |<--- SUBSCRIBE (video) ---|
      |<-- SUBSCRIBE (video) -------|                          |
      |--- SUBSCRIBE_OK ----------->|---- SUBSCRIBE_OK ------->|
      |                              |<--- SUBSCRIBE (audio) ---|
      |<-- SUBSCRIBE (audio) -------|                          |
      |--- SUBSCRIBE_OK ----------->|---- SUBSCRIBE_OK ------->|
      |                              |                          |
      |--- OBJECT (video I-frame) ->|---- OBJECT ------------->| writes video
      |--- OBJECT (audio AAC) ----->|---- OBJECT ------------->| writes audio
      |--- OBJECT (video P-frame) ->|---- OBJECT ------------->| writes video
      :                              :                          :
      |--- FIN (end of track) ----->|---- FIN ---------------->| closes output.flv
```

```bash
# Terminal 1: Start relay
moqrelayserver --insecure --port 4433

# Terminal 2: Publish FLV file
moqflvstreamerclient --insecure \
  --input_flv_file /tmp/input.flv \
  --connect_url "https://localhost:4433/moq-relay" \
  --track_namespace "live" \
  --video_track_name "video" \
  --audio_track_name "audio"

# Terminal 3: Subscribe and save to file
moqflvreceiverclient --insecure \
  --connect_url "https://localhost:4433/moq-relay" \
  --track_namespace "live" \
  --video_track_name "video" \
  --audio_track_name "audio" \
  --flv_outpath /tmp/output.flv
```

### Workflow 4: Test suite

```
  moqtest_server                              moqtest_client
  (publisher)                                 (subscriber)
      |                                            |
      |  listening on :9999                        |
      |<------------ QUIC/WT connect :9999 --------|
      |<------------ CLIENT_SETUP -----------------|
      |------------- SERVER_SETUP ---------------->|
      |                                            |
      |  request="subscribe":                      |
      |<------------ SUBSCRIBE --------------------|
      |------------- SUBSCRIBE_OK ---------------->|
      |------------- OBJECT (test data) ---------->| validates content
      |------------- OBJECT ---------------------->| validates content
      :                                            :
      |  request="fetch":                          |
      |<------------ FETCH (group 0..10) ----------|
      |------------- OBJECT (group 0, obj 0) ---->| validates content
      |------------- OBJECT (group 0, obj 1) ---->| validates content
      :                                            :
      |------------- FETCH_OK -------------------->| done
```

```bash
# Terminal 1: Start test server
moqtest_server --port 9999

# Terminal 2: Run test client (subscribe)
moqtest_client --url "http://localhost:9999" --request "subscribe"

# Or fetch a range
moqtest_client --url "http://localhost:9999" --request "fetch" \
  --start_group 0 --start_object 0 --last_group 10

# With raw QUIC and logging
moqtest_server --port 9999 --quic_transport --log --mlog_path ./server.log
moqtest_client --url "http://localhost:9999" --request "subscribe" \
  --quic_transport --log --mlog_path ./client.log
```

### Workflow 5: Picoquic Date Server + Text Client (std-mode)

Uses the picoquic transport with the callback-based MoQSessionCompat (no Folly/mvfst).
Build first with direct CMake (see [Std-mode + picoquic](#2-std-mode--picoquic-minimal-dependencies)).
Binaries are in the build tree at `_build_std/moxygen/samples/`.

```
  picodateserver                              picotextclient
  (server+publisher, picoquic)                (subscriber, picoquic)
      |                                            |
      |  listening on :4433                        |
      |<------------ QUIC connect :4433 -----------|
      |<------------ CLIENT_SETUP -----------------|
      |------------- SERVER_SETUP ---------------->|
      |<------------ SUBSCRIBE --------------------|
      |------------- SUBSCRIBE_OK ---------------->|
      |                                            |
      |------------- OBJECT (date/time) ---------->| stdout: "2026-02-04 14:39:00"
      |------------- OBJECT ---------------------->| stdout: "2026-02-04 14:39:01"
      :                                            :
```

```bash
# Generate TLS certs (required for QUIC)
cd _build_std
openssl genrsa -out key.pem 2048
openssl req -new -x509 -key key.pem -out cert.pem -days 365 -subj "/CN=localhost"
```

#### Run: Raw QUIC (default)

```bash
# Terminal 1: Start date server (raw QUIC - default)
./_build_std/moxygen/samples/date/picodateserver \
  -p 4433 \
  --cert _build_std/cert.pem \
  --key _build_std/key.pem \
  -n moq-date \
  -m spg \
  -t quic

# Terminal 2: Connect client (raw QUIC)
./_build_std/moxygen/samples/text-client/picotextclient \
  -H localhost \
  -p 4433 \
  -n moq-date \
  --track date \
  -T quic \
  -k
```

#### Run: WebTransport (HTTP/3)

```bash
# Terminal 1: Start date server (WebTransport)
./_build_std/moxygen/samples/date/picodateserver \
  -p 4433 \
  --cert _build_std/cert.pem \
  --key _build_std/key.pem \
  -n moq-date \
  -m spg \
  -t webtransport

# Terminal 2: Connect client (WebTransport)
./_build_std/moxygen/samples/text-client/picotextclient \
  -H localhost \
  -p 4433 \
  -n moq-date \
  --track date \
  -T webtransport \
  -k
```

**Expected Output:** Client prints seconds counter: `00`, `01`, `02`, ... (updates each second)

**picodateserver flags:**
| Flag | Default | Description |
|------|---------|-------------|
| `--cert, -c` | (required) | TLS certificate file |
| `--key, -k` | (required) | TLS private key file |
| `--port, -p` | 4433 | Server port |
| `--ns, -n` | moq-date | Track namespace |
| `--mode, -m` | spg | Transmit mode: spg, spo, datagram |
| `--transport, -t` | quic | Transport mode: quic, webtransport |

**picotextclient flags:**
| Flag | Default | Description |
|------|---------|-------------|
| `--host, -H` | (required) | Server hostname |
| `--port, -p` | 4433 | Server port |
| `--ns, -n` | moq-date | Track namespace |
| `--track` | date | Track name |
| `--transport, -T` | quic | Transport mode: quic, webtransport |
| `--insecure, -k` | false | Skip TLS cert validation |
| `--timeout` | 5000 | Connect timeout in ms |

### Workflow 6: Picoquic Relay + Date Server + Text Client (std-mode)

Uses picoquic transport with the callback-based MoQSessionCompat for a full relay setup.

```
  picodateserver            picorelayserver           picotextclient
  (publisher)                   (relay)                (subscriber)
      |                            |                        |
      |--- QUIC connect :4443 ---->|                        |
      |--- CLIENT_SETUP ---------->|                        |
      |<-- SERVER_SETUP -----------|                        |
      |--- PUBLISH_NAMESPACE ----->|                        |
      |<-- PUBLISH_NAMESPACE_OK ---|                        |
      |                            |<-- QUIC connect :4443 -|
      |                            |<-- CLIENT_SETUP -------|
      |                            |--- SERVER_SETUP ------>|
      |                            |<-- SUBSCRIBE ----------|
      |<-- SUBSCRIBE --------------|                        |
      |--- SUBSCRIBE_OK ---------->|--- SUBSCRIBE_OK ------>|
      |                            |                        |
      |--- OBJECT (date/time) ---->|--- OBJECT ------------>| "2026-02-16 12:00:00"
      :                            :                        :
```

```bash
# Terminal 1: Start relay server (raw QUIC)
./_build_std/moxygen/samples/relay/picorelayserver \
  -p 4443 \
  -c _build_std/cert.pem \
  -k _build_std/key.pem \
  -t quic

# Terminal 2: Start date server (publishes to relay via raw QUIC)
./_build_std/moxygen/samples/date/picodateserver \
  -p 4433 \
  --cert _build_std/cert.pem \
  --key _build_std/key.pem \
  -n moq-date \
  -m spg \
  -r moq://localhost:4443 \
  --insecure

# Terminal 3: Subscribe via relay (raw QUIC)
./_build_std/moxygen/samples/text-client/picotextclient \
  -H localhost \
  -p 4443 \
  -n moq-date \
  --track date \
  -T quic \
  --insecure
```

**Key points:**
- Use `moq://` scheme for raw QUIC transport (uses `moq-00` ALPN)
- Use `https://` scheme for WebTransport (uses `h3` ALPN)
- The relay and date server must use different ports (relay: 4443, server: 4567)
- The client connects to the relay, not the date server

### Workflow 7: Picoquic Relay + Date Server + Text Client with WebTransport (std-mode)

Same as Workflow 6 but using WebTransport (HTTP/3) instead of raw QUIC.

```bash
# Terminal 1: Start relay server (WebTransport)
./_build_std/moxygen/samples/relay/picorelayserver \
  -p 4443 \
  -c _build_std/cert.pem \
  -k _build_std/key.pem \
  -t webtransport

# Terminal 2: Start date server (publishes to relay via WebTransport)
./_build_std/moxygen/samples/date/picodateserver \
  -p 4433 \
  --cert _build_std/cert.pem \
  --key _build_std/key.pem \
  -n moq-date \
  -m spg \
  -r https://localhost:4443/moq \
  --insecure

# Terminal 3: Subscribe via relay (WebTransport)
./_build_std/moxygen/samples/text-client/picotextclient \
  -H localhost \
  -p 4443 \
  -n moq-date \
  --track date \
  -T webtransport \
  -k
```

**Expected Output:** Client prints seconds counter through relay.

**Differences from raw QUIC (Workflow 6):**

| Component | Raw QUIC | WebTransport |
|-----------|----------|--------------|
| Relay | (default) | `--transport webtransport` |
| Date Server URL | `moq://...` | `https://...` |
| Client | (default) | `--transport webtransport` |
| ALPN | `moq-00` | `h3` |

### Workflow 8: Advanced Std-mode Tests (WebTransport & Interop)

This section provides comprehensive test commands for the picoquic std-mode implementation,
covering relay mode, datagram delivery, cross-transport mode, and cross-flavor interoperability.

**Prerequisites:**
```bash
# Build std-mode (if not already done)
cd /Users/suhas/work/code/moq/moxygen
mkdir -p _build_std && cd _build_std
cmake .. -DMOXYGEN_USE_FOLLY=OFF -DMOXYGEN_QUIC_BACKEND=picoquic -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build . --parallel

# Generate TLS certs (if not already done)
cd /Users/suhas/work/code/moq/moxygen
openssl req -x509 -newkey rsa:2048 -keyout key.pem -out cert.pem -days 365 -nodes -subj "/CN=localhost"
```

---

#### Test 1: Relay Mode (Date Server → Relay → Client) - WebTransport

Full relay setup where the date server publishes through the relay.

```bash
# Terminal 1: Start relay server (WebTransport)
./_build_std/moxygen/samples/relay/picorelayserver \
  -p 4443 \
  -c cert.pem \
  -k key.pem \
  -t webtransport

# Terminal 2: Start date server publishing TO the relay
./_build_std/moxygen/samples/date/picodateserver \
  -p 4433 \
  -c cert.pem \
  -k key.pem \
  -n moq-date \
  -m spg \
  -r https://localhost:4443/moq \
  --insecure

# Terminal 3: Subscribe via relay
./_build_std/moxygen/samples/text-client/picotextclient \
  -H localhost \
  -p 4443 \
  -n moq-date \
  --track date \
  -T webtransport \
  --insecure
```

**Expected:** Client receives date/time objects flowing through the relay.

---

#### Test 2: Relay Mode (Date Server → Relay → Client) - Raw QUIC

Same as Test 1 but using raw QUIC transport (`moq-00` ALPN).

```bash
# Terminal 1: Start relay server (raw QUIC - default)
./_build_std/moxygen/samples/relay/picorelayserver \
  -p 4443 \
  -c cert.pem \
  -k key.pem \
  -t quic

# Terminal 2: Start date server publishing TO the relay (raw QUIC)
./_build_std/moxygen/samples/date/picodateserver \
  -p 4433 \
  -c cert.pem \
  -k key.pem \
  -n moq-date \
  -m spg \
  -r moq://localhost:4443 \
  --insecure

# Terminal 3: Subscribe via relay (raw QUIC)
./_build_std/moxygen/samples/text-client/picotextclient \
  -H localhost \
  -p 4443 \
  -n moq-date \
  --track date \
  -T quic \
  --insecure
```

**Expected:** Client receives date/time objects via raw QUIC.

---

#### Test 3: Datagram Delivery Mode

Test datagram-based object delivery instead of streams.

```bash
# Terminal 1: Start date server with datagram mode (direct, no relay)
./_build_std/moxygen/samples/date/picodateserver \
  -p 4433 \
  -c cert.pem \
  -k key.pem \
  -n moq-date \
  -m datagram \
  -t webtransport

# Terminal 2: Connect client
./_build_std/moxygen/samples/text-client/picotextclient \
  -H localhost \
  -p 4433 \
  -n moq-date \
  --track date \
  -T webtransport \
  --insecure
```

**Expected:** Date objects delivered via datagrams (lower latency, unreliable).

---

#### Test 4: Datagram Delivery via Relay

Datagram delivery through a relay.

```bash
# Terminal 1: Start relay (WebTransport)
./_build_std/moxygen/samples/relay/picorelayserver \
  -p 4443 \
  -c cert.pem \
  -k key.pem \
  -t webtransport

# Terminal 2: Date server with datagram mode, publishing to relay
./_build_std/moxygen/samples/date/picodateserver \
  -p 4433 \
  -c cert.pem \
  -k key.pem \
  -n moq-date \
  -m datagram \
  -r https://localhost:4443/moq \
  --insecure

# Terminal 3: Client subscribes via relay
./_build_std/moxygen/samples/text-client/picotextclient \
  -H localhost \
  -p 4443 \
  -n moq-date \
  --track date \
  -T webtransport \
  --insecure
```

---

#### Test 5: Cross-Transport Mode (Mixed QUIC and WebTransport)

Test scenarios where different components use different transport modes.
**Note:** This requires the relay to accept both transport modes on the same port.

**Scenario A: WebTransport server ← Raw QUIC client**
```bash
# Terminal 1: Start date server with WebTransport
./_build_std/moxygen/samples/date/picodateserver \
  -p 4433 \
  -c cert.pem \
  -k key.pem \
  -n moq-date \
  -m spg \
  -t webtransport

# Terminal 2: Try connecting with raw QUIC client
# NOTE: This will FAIL - ALPN mismatch (moq-00 vs h3)
./_build_std/moxygen/samples/text-client/picotextclient \
  -H localhost \
  -p 4433 \
  -n moq-date \
  --track date \
  -T quic \
  --insecure
```

**Expected:** Connection fails due to ALPN mismatch. This demonstrates that
raw QUIC (`moq-00`) and WebTransport (`h3`) are NOT directly interoperable.

**Scenario B: Relay bridges different transport modes**

Currently the picoquic relay accepts ONE transport mode per instance.
For cross-transport bridging, use the mvfst relay (see Test 6).

---

#### Test 6: Cross-Flavor Interop (Picoquic ↔ mvfst)

Test interoperability between picoquic (std-mode) and mvfst (Folly) implementations.
Both must use WebTransport (`h3` ALPN) for interop.

**Scenario A: Picoquic client → mvfst relay → mvfst publisher**

```bash
# Terminal 1: Start mvfst relay (from Folly build)
# Assumes _build/installed/moxygen/bin is in PATH
moqrelayserver --insecure --port 4433

# Terminal 2: Start mvfst date publisher (connects to relay)
moqdateserver --insecure --port 4434 \
  --relay_url "https://localhost:4433/moq-relay" \
  --ns "moq-date"

# Terminal 3: Connect with PICOQUIC client via WebTransport
./_build_std/moxygen/samples/text-client/picotextclient \
  -H localhost \
  -p 4433 \
  -n moq-date \
  --track date \
  -T webtransport \
  --insecure
```

**Expected:** Picoquic client receives date objects from mvfst publisher via mvfst relay.

---

**Scenario B: mvfst client → picoquic server**

```bash
# Terminal 1: Start picoquic date server with WebTransport
./_build_std/moxygen/samples/date/picodateserver \
  -p 4433 \
  -c cert.pem \
  -k key.pem \
  -n moq-date \
  -m spg \
  -t webtransport

# Terminal 2: Connect with mvfst client
moqtextclient --insecure \
  --connect_url "https://localhost:4433/moq-date" \
  --track_namespace "moq-date" \
  --track_name "date"
```

**Expected:** mvfst client receives date objects from picoquic server.

---

**Scenario C: Picoquic publisher → mvfst relay ← Picoquic subscriber**

Full cross-flavor: both publisher and subscriber are picoquic, relay is mvfst.

```bash
# Terminal 1: Start mvfst relay
moqrelayserver --insecure --port 4433

# Terminal 2: Start PICOQUIC date server publishing TO mvfst relay
./_build_std/moxygen/samples/date/picodateserver \
  -p 4434 \
  -c cert.pem \
  -k key.pem \
  -n moq-date \
  -m spg \
  -r https://localhost:4433/moq-relay \
  --insecure

# Terminal 3: PICOQUIC client subscribes via mvfst relay
./_build_std/moxygen/samples/text-client/picotextclient \
  -H localhost \
  -p 4433 \
  -n moq-date \
  --track date \
  -T webtransport \
  --insecure
```

**Expected:** Picoquic client receives objects from picoquic publisher, routed through mvfst relay.

---

#### Test 7: All Delivery Modes Comparison

Compare the three delivery modes: stream-per-group, stream-per-object, and datagram.

```bash
# Stream Per Group (spg) - default, one stream per minute
./_build_std/moxygen/samples/date/picodateserver \
  -p 4433 -c cert.pem -k key.pem -n moq-date -m spg -t webtransport

# Stream Per Object (spo) - one stream per second
./_build_std/moxygen/samples/date/picodateserver \
  -p 4433 -c cert.pem -k key.pem -n moq-date -m spo -t webtransport

# Datagram - unreliable, lowest latency
./_build_std/moxygen/samples/date/picodateserver \
  -p 4433 -c cert.pem -k key.pem -n moq-date -m datagram -t webtransport
```

---

#### Quick Reference: URL Schemes and Transport Modes

| URL Scheme | Transport Mode | ALPN | Flag (server) | Flag (client) |
|------------|----------------|------|---------------|---------------|
| `moq://host:port` | Raw QUIC | `moq-00` | `-t quic` | `-T quic` |
| `https://host:port/path` | WebTransport | `h3` | `-t webtransport` | `-T webtransport` |

**Interoperability Matrix:**

| Server Transport | Client Transport | Result |
|------------------|------------------|--------|
| WebTransport | WebTransport | ✅ Works |
| Raw QUIC | Raw QUIC | ✅ Works |
| WebTransport | Raw QUIC | ❌ ALPN mismatch |
| Raw QUIC | WebTransport | ❌ ALPN mismatch |
| mvfst (WebTransport) | picoquic (WebTransport) | ✅ Works |
| picoquic (WebTransport) | mvfst (WebTransport) | ✅ Works |

---

### Debug Logging

All binaries use glog. Set verbosity with `--v` and per-module with `--vmodule`:

```bash
# Verbose logging (higher = more verbose)
moqdateserver --insecure --port 4433 --ns "moq-date" --v=2

# Per-module verbosity (e.g. debug MoQSession only)
moqdateserver --insecure --port 4433 --ns "moq-date" \
  --vmodule="MoQSession=4,MoQCodec=3"

# MoQ protocol logging to file (available on most binaries)
moqtextclient --insecure \
  --connect_url "https://localhost:4433/moq-date" \
  --track_namespace "moq-date" \
  --track_name "date" \
  --mlog_path /tmp/moq_client.log
```

---

## Running Tests

### Unit Tests (Folly + mvfst)

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

### Std-mode Tests (picoquic backend)

These tests work with `MOXYGEN_USE_FOLLY=OFF` and `MOXYGEN_QUIC_BACKEND=picoquic` builds.
Run from the `_build_std` directory after building with direct CMake.

```bash
cd _build_std

# Framer tests (15 tests) - frame encoding/decoding
./moxygen/test/MoQFramerTestStd

# Codec tests (3 tests) - control message codec
./moxygen/test/MoQCodecTestStd

# Test framework tests (20 tests) - executor, mock transport, async helpers
./moxygen/test/MoQTestFrameworkTest

# Session tests (26 tests) - callback-based session tests
./moxygen/test/MoQSessionTestCompat

# Integration tests (11 tests) - real picoquic client/server
./moxygen/test/integration/PicoquicIntegrationTest
```

**Std-mode test binaries:**
| Binary | Tests | Description |
|--------|-------|-------------|
| `MoQFramerTestStd` | 15 | Frame serialization/parsing |
| `MoQCodecTestStd` | 3 | Control message codec |
| `MoQTestFrameworkTest` | 20 | TestExecutor, MockMoQTransport, AsyncTestHelper |
| `MoQSessionTestCompat` | 26 | Callback-based session, frame builders |
| `PicoquicIntegrationTest` | 11 | End-to-end with real network (requires TLS certs) |

**Integration tests** require TLS certificates. They automatically look for picoquic's
test certs in `_build_std/_deps/picoquic-src/certs/`. Tests skip gracefully if certs
are not found.

**Run all std-mode tests:**
```bash
cd _build_std
for test in \
    moxygen/test/MoQFramerTestStd \
    moxygen/test/MoQCodecTestStd \
    moxygen/test/MoQTestFrameworkTest \
    moxygen/test/MoQSessionTestCompat \
    moxygen/test/integration/PicoquicIntegrationTest; do
  echo "=== Running $test ===" && ./$test || echo "FAILED: $test"
done
```

---

## Key Flags Reference

| Flag | Binaries | Description |
|------|----------|-------------|
| `--insecure` | all | Skip TLS cert validation (dev only) |
| `--port N` | servers | Listen port |
| `--connect_url URL` | clients | Server/relay URL to connect to |
| `--track_namespace NS` | clients | Track namespace (e.g. "moq-date") |
| `--track_name NAME` | moqtextclient | Track name (e.g. "date") |
| `--relay_url URL` | moqdateserver | Relay to publish through |
| `--enable_cache` | moqrelayserver | Enable relay-side caching |
| `--mode spg\|spo\|datagram` | moqdateserver | Transmission mode |
| `--fetch` | moqtextclient, moqflvreceiverclient | Use fetch instead of subscribe |
| `--sg N --so N --eg N` | moqtextclient | Start group/object, end group |
| `--delivery_timeout MS` | most | Max delivery time (0=disabled) |
| `--mlog_path PATH` | most | Write MoQ protocol logs to file |
| `--quic_transport` | most | Use raw QUIC instead of WebTransport |

### Track Naming

```
Namespace:  hierarchical path, e.g. "live", "live/channel1", "moq-date"
Track name: individual name, e.g. "video", "audio", "date"
Delimiter:  "/" by default (--track_namespace_delimiter)
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

### Transport Mode Support (Cross-Flavor Interoperability)

The picoquic-based samples support two transport modes, enabling interoperability between
different build flavors (e.g., picoquic client connecting to mvfst relay):

| Mode | ALPN | Description |
|------|------|-------------|
| `quic` | `moq-00` | Raw QUIC with MoQ-specific ALPN (default) |
| `webtransport` | `h3` | WebTransport over HTTP/3 |

**Cross-flavor interop examples:**

```bash
# Picoquic client (std-mode) → mvfst relay (Folly) via WebTransport
./picotextclient --host localhost --port 4433 --transport webtransport --insecure

# mvfst client (Folly) → picoquic server (std-mode) via WebTransport
./moqtextclient --insecure --connect_url "https://localhost:4433/moq-date"
```

**Key files:**
- `moxygen/transports/TransportMode.h` - `TransportMode` enum (QUIC vs WEBTRANSPORT)
- `moxygen/transports/PicoquicH3Transport.h/cpp` - HTTP/3 transport using picoquic's h3zero
- `moxygen/transports/PicoquicWebTransport.h/cpp` - Raw QUIC transport wrapper

**How it works:**
1. `TransportMode::QUIC` uses ALPN `moq-00` and sends MoQ frames directly over QUIC streams
2. `TransportMode::WEBTRANSPORT` uses ALPN `h3` and wraps streams in HTTP/3 framing
3. Both modes use the same `compat::WebTransportInterface` abstraction
4. The relay (mvfst or picoquic) accepts both modes on the same port

**ALPN selection:**
```cpp
// In PicoquicMoQClient/Server config
if (config.transportMode == TransportMode::WEBTRANSPORT) {
  alpn = "h3";  // HTTP/3 for WebTransport
} else {
  alpn = "moq-00";  // Raw QUIC with MoQ ALPN
}
```

---

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

**zlib download fails (zlib.net connection refused / 404)**
- zlib.net removes old version URLs when new versions are released. This is a
  recurring problem that has broken builds for many projects (TensorFlow, Bazel,
  protobuf, etc.)
- **Fix:** Install zlib via your system package manager and use `--allow-system-packages`:
  ```bash
  brew install zlib   # macOS
  # apt install zlib1g-dev   # Ubuntu/Debian
  ```
  Then rebuild. getdeps will use the system zlib instead of downloading.
- Note: moxygen doesn't use zlib directly. It's a transitive dependency needed
  by fizz (TLS) and proxygen (HTTP compression).

**Build stops after folly (no fizz/wangle/mvfst/proxygen/moxygen)**
- Check `_build/installed/` to see which dependencies were built. If the chain
  stops early (e.g. at folly), a dependency download likely failed silently.
- Re-run with `--allow-system-packages` and ensure system deps are installed:
  ```bash
  brew install zlib openssl@3 libsodium lz4 xz zstd icu4c fmt
  ./build/fbcode_builder/getdeps.py build --src-dir=moxygen:. moxygen \
    --allow-system-packages --scratch-path _build \
    --extra-cmake-defines '{"CMAKE_POLICY_VERSION_MINIMUM":"3.5"}'
  ```
- Add `--verbose` to see where exactly the build fails.

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

**`dyld: Library not loaded: @rpath/libcares.2.dylib` (or similar @rpath errors)**
- The binaries use `@rpath` to find dynamic libraries, pointing to `_build/deps/lib/`
- This directory doesn't exist after a fresh build. Create it and symlink all dylibs:
  ```bash
  mkdir -p _build/deps/lib
  find "$(pwd)/_build/installed" -name "*.dylib" -exec ln -sf {} _build/deps/lib/ \;
  ```
- **Important:** You must use `$(pwd)/_build/installed` (with absolute path) instead of just
  `_build/installed`. Without the absolute path, the symlinks will be relative and won't
  resolve correctly when the binary runs.
- You need to re-run this after every clean rebuild.
- If you already ran it with relative paths, delete and recreate:
  ```bash
  rm -rf _build/deps/lib
  mkdir -p _build/deps/lib
  find "$(pwd)/_build/installed" -name "*.dylib" -exec ln -sf {} _build/deps/lib/ \;
  ```

**`SubscribeError code=4 reason=no such namespace or track`**
- The namespace hasn't been published to the relay yet.
- If using a relay: make sure the date server uses `--relay_url` to connect to the relay
  as a publisher. Without it, the date server runs standalone and the relay doesn't know
  about the namespace.
- If connecting directly: make sure the `--connect_url` path matches the server's
  namespace (e.g. `/moq-date`), not `/moq-relay`.

**`prefix not found in publishNamespace tree` (relay log)**
- No publisher has registered a matching namespace with the relay.
- Check that the date server started with `--relay_url` pointing to the relay and is running.

**`Non-200 response: 404`**
- The URL path doesn't match any endpoint on the server.
- `/moq-relay` is the relay endpoint. Date server standalone serves at `/<namespace>` (e.g. `/moq-date`).
- Don't mix relay URLs with direct server URLs.

**Port conflict / `ApplicationError: 268`**
- Two servers cannot listen on the same port. When using a relay, the date server and
  relay must use different `--port` values (e.g. relay on 4433, date server on 4434).

**Connection refused**
- Ensure server is running on the specified port
- Check firewall settings

**Certificate errors**
- Use `--insecure` flag for testing with self-signed certs
- Or provide proper certificate chain

**Protocol version mismatch**
- Both client and server must support compatible MoQ versions
- Check `MoQVersions.h` for supported versions

**`Object ID not advancing` error in MoQDateServer (FIXED in commit c1b5c59)**
- **Symptom:** When running MoQDateServer with WebTransport, crossing a minute boundary
  caused errors like:
  ```
  E0209 12:58:00.195347 MoQSession.cpp:598] Object ID not advancing header_.id=25 objectID=0
  ```
- **Root cause:** The `publishDate()` function in `MoQDateServer.cpp` didn't detect
  minute (group) boundary changes. When the minute changed (e.g., from 57 to 58),
  the old subgroupPublisher was reused for the new group, causing new objects
  (starting at ID 0) to conflict with the previous group's object IDs.
- **Fix:** Added `currentGroup` tracking to detect group changes and properly end
  the previous group before starting a new subgroup:
  ```cpp
  // In publishDate(): check for group change
  if (!subgroupPublisher || group != currentGroup) {
    if (subgroupPublisher) {
      subgroupPublisher->endOfGroup(61);  // End previous group
    }
    subgroupPublisher = forwarder_.beginSubgroup(group, subgroup, 0).value();
    currentGroup = group;
  }
  ```
- **Verification:** Run the client across a minute boundary - you should see:
  ```
  58
  59
  ObjectStatus=3           <-- END_OF_GROUP signal
  2026-02-14 17:57:        <-- New minute prefix
  0                        <-- Object IDs correctly restart at 0
  1
  ...
  ```

**ICU library not found on macOS (`ld: library 'icudata' not found`)**
- This happens with local Folly+mvfst builds when ICU is installed via Homebrew
  but the linker doesn't know where to find it
- **Fix:** Set `LIBRARY_PATH` before building:
  ```bash
  export LIBRARY_PATH="/opt/homebrew/opt/icu4c@78/lib:$LIBRARY_PATH"
  cmake --build . -j4
  ```
- Alternatively, add to your shell profile to make it persistent:
  ```bash
  # Add to ~/.zshrc or ~/.bashrc
  export LIBRARY_PATH="/opt/homebrew/opt/icu4c@78/lib:$LIBRARY_PATH"
  ```
- Note: Check the actual ICU version installed (`ls /opt/homebrew/opt/ | grep icu`)
  and adjust the path accordingly (e.g., `icu4c@76`, `icu4c@78`)

**Local Folly+mvfst Build Setup (vs getdeps)**

For development, you may want a local build directory that reuses pre-built
dependencies from a previous getdeps build. This is faster for iterative
development but requires some manual setup.

```bash
# Assuming you have a previous getdeps build in _build_folly_mvfst/

# 1. Create a local build directory
mkdir -p _build_local && cd _build_local

# 2. Point CMAKE_PREFIX_PATH to installed dependencies
cmake .. \
  -DCMAKE_PREFIX_PATH="/path/to/moxygen/_build_folly_mvfst/installed/folly;/path/to/moxygen/_build_folly_mvfst/installed/proxygen;/path/to/moxygen/_build_folly_mvfst/installed/mvfst;/path/to/moxygen/_build_folly_mvfst/installed/fizz;/path/to/moxygen/_build_folly_mvfst/installed/wangle" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo

# 3. Build with ICU library path set (macOS)
export LIBRARY_PATH="/opt/homebrew/opt/icu4c@78/lib:$LIBRARY_PATH"
cmake --build . -j4
```

**Common issues with local builds:**
- Missing symbols for newer features: The compat branch may add virtual methods
  that need implementations in MoQSession.cpp (for Folly+mvfst) as well as
  MoQSessionCompat.cpp (for std-mode/picoquic)
- Library path issues: macOS requires `LIBRARY_PATH` for ICU; Linux may need
  `LD_LIBRARY_PATH` for runtime

---

## File Locations

| Component | Path |
|-----------|------|
| Core library | `moxygen/` |
| Compatibility layer | `moxygen/compat/` |
| Transport adapters | `moxygen/transports/` |
| Transport mode enum | `moxygen/transports/TransportMode.h` |
| Picoquic raw QUIC transport | `moxygen/transports/PicoquicWebTransport.h/cpp` |
| Picoquic HTTP/3 transport | `moxygen/transports/PicoquicH3Transport.h/cpp` |
| Picoquic client/server | `moxygen/transports/PicoquicMoQClient.h/cpp`, `PicoquicMoQServer.h/cpp` |
| Relay implementation | `moxygen/relay/` |
| Samples (Folly+mvfst) | `moxygen/samples/date/MoQDateServer.cpp`, `MoQTextClient.cpp` |
| Samples (picoquic) | `moxygen/samples/date/PicoDateServer.cpp`, `PicoTextClient.cpp` |
| Callback-based session | `moxygen/MoQSessionCompat.cpp` |
| Coroutine-based session | `moxygen/MoQSession.cpp` |
| Simple executor (std-mode) | `moxygen/events/MoQSimpleExecutor.cpp` |
| Tests (Folly+mvfst) | `moxygen/test/MoQFramerTest.cpp`, `MoQSessionTest.cpp`, etc. |
| Tests (std-mode) | `moxygen/test/MoQFramerTestStd.cpp`, `MoQSessionTestCompat.cpp`, etc. |
| Test framework (std-mode) | `moxygen/test/TestExecutor.h`, `MockMoQTransport.h`, `MoQTestFixtureCompat.h` |
| Integration tests | `moxygen/test/integration/PicoquicIntegrationTest.cpp` |
| Logging | `moxygen/mlog/` |
| Build config | `CMakeLists.txt`, `moxygen/CMakeLists.txt` |

---

## Build Output Details

### Moxygen Libraries (Folly + mvfst build)

```
$ ls -lh _build/installed/moxygen/lib/*.a
-rw-r--r--  2.3M  libflvparser.a
-rw-r--r--  5.7M  libmlogger.a
-rw-r--r--  3.7M  libmoqcache.a
-rw-r--r--  466K  libmoqcompat.a
-rw-r--r--  558K  libmoqmi.a
-rw-r--r--   14M  libmoqrelay.a
-rw-r--r--  4.9M  libmoqrelaysession.a
-rw-r--r--  331K  libmoqtest_utils.a
-rw-r--r--  7.2M  libmoqtransport_mvfst_stdmode.a   # Hybrid mode adapter
-rw-r--r--  1.7M  libmoqtransport_proxygen.a
-rw-r--r--   17M  libmoxygen.a                      # Core library
```

### Object Files in Key Libraries

```bash
# Core library
$ ar -t libmoxygen.a
MoQFramer.cpp.o
MoQCodec.cpp.o
MoQSession.cpp.o
MoQTokenCache.cpp.o
MoQTypes.cpp.o
MoQVersions.cpp.o
MoQDeliveryTimer.cpp.o

# Hybrid mode adapter
$ ar -t libmoqtransport_mvfst_stdmode.a
MvfstStdModeAdapter.cpp.o

# Proxygen adapter
$ ar -t libmoqtransport_proxygen.a
ProxygenWebTransportAdapter.cpp.o
```

### Executables

```
$ ls -lh _build/installed/moxygen/bin/
-rwxr-xr-x   11M  moqdateserver
-rwxr-xr-x  8.5M  moqflvreceiverclient
-rwxr-xr-x  8.7M  moqflvstreamerclient
-rwxr-xr-x  8.5M  moqperf_test_client
-rwxr-xr-x   11M  moqrelayserver
-rwxr-xr-x  8.5M  moqtest_client
-rwxr-xr-x   11M  moqtest_server
-rwxr-xr-x  8.6M  moqtextclient
```

### Dynamic Library Dependencies (macOS)

```bash
$ otool -L _build/installed/moxygen/bin/moqrelayserver
/Users/.../moqrelayserver:
    /opt/homebrew/opt/zstd/lib/libzstd.1.dylib
    /opt/homebrew/opt/libsodium/lib/libsodium.26.dylib
    @rpath/libcares.2.dylib
    /usr/lib/libresolv.9.dylib
    .../libevent-2.1.7.dylib
    /opt/homebrew/opt/fmt/lib/libfmt.12.dylib
    /usr/lib/libbz2.1.0.dylib
    /opt/homebrew/opt/xz/lib/liblzma.5.dylib
    /opt/homebrew/opt/lz4/lib/liblz4.1.dylib
    /usr/lib/libc++abi.dylib
    @rpath/libglog.0.dylib
    /opt/homebrew/opt/icu4c@78/lib/libicudata.78.dylib
    /opt/homebrew/opt/icu4c@78/lib/libicui18n.78.dylib
    /opt/homebrew/opt/icu4c@78/lib/libicuuc.78.dylib
    @rpath/libgflags.2.2.dylib
    /opt/homebrew/opt/openssl@3/lib/libssl.3.dylib
    /opt/homebrew/opt/openssl@3/lib/libcrypto.3.dylib
    /usr/lib/libc++.1.dylib
    /usr/lib/libSystem.B.dylib
```

**Note:** Libraries with `@rpath` need the rpath set correctly. For local testing:
```bash
mkdir -p _build/deps/lib
find "$(pwd)/_build/installed" -name "*.dylib" -exec ln -sf {} _build/deps/lib/ \;
export DYLD_LIBRARY_PATH="$(pwd)/_build/deps/lib:$DYLD_LIBRARY_PATH"
```

### Key Symbols in Hybrid Adapter

```bash
$ nm -g libmoqtransport_mvfst_stdmode.a | grep "MvfstStdMode" | c++filt
# Key classes:
moxygen::transports::MvfstStdModeAdapter::create(Config)
moxygen::transports::MvfstStdModeAdapter::connect(std::function<void(bool)>)
moxygen::transports::MvfstStdModeAdapter::createUniStream()
moxygen::transports::MvfstStdModeAdapter::createBidiStream()
moxygen::transports::MvfstStdModeAdapter::sendDatagram(...)
moxygen::transports::MvfstStdModeAdapter::closeSession(uint32_t)
moxygen::transports::MvfstStdModeServer::create(Config)
moxygen::transports::MvfstStdModeServer::start(std::function<void(bool)>)
moxygen::transports::MvfstStdModeServer::stop()
```

---

## Dependency Notes

### Why Folly/Proxygen Are Built Even With `MOXYGEN_USE_FOLLY=OFF`

The `getdeps.py` build system uses a **manifest file** (`build/fbcode_builder/manifests/moxygen`)
that unconditionally lists all dependencies:

```
[dependencies]
zlib
gperf
folly
fizz
wangle
mvfst
proxygen
```

**getdeps.py does NOT understand CMake options.** It always builds every dependency listed
in `[dependencies]`, regardless of what `-DMOXYGEN_USE_FOLLY=OFF` says. The CMake flag only
controls what moxygen itself links against --- but getdeps has already built the full chain
by the time CMake runs.

**Impact:** Extra build time (~20-30 min for the full chain), but the unused libraries
simply sit in `_build/installed/` and are not linked into the final moxygen binaries.

**Potential fix:** Create a separate manifest (`manifests/moxygen-std`) with minimal deps
for std-mode builds. Not done yet.

### Dependency Source Summary

| Dependency | Source | Used By |
|-----------|--------|---------|
| folly | getdeps manifest | Folly + mvfst, Folly + picoquic |
| fizz | getdeps manifest | Folly + mvfst only (TLS for mvfst) |
| wangle | getdeps manifest | Folly + mvfst only |
| mvfst | getdeps manifest | Folly + mvfst only |
| proxygen | getdeps manifest | Folly + mvfst only |
| picoquic | CMake FetchContent | Std-mode + picoquic, Folly + picoquic |
| picotls | CMake FetchContent (via picoquic) | Std-mode + picoquic, Folly + picoquic |
| openssl | System or getdeps | All flavors |
| mbedtls | CMake FetchContent | Std-mode + picoquic (optional) |
| zlib | getdeps or system (`brew install zlib`) | Transitive only (fizz, proxygen) |
| boost | getdeps manifest | Via folly |
| glog/gflags | getdeps manifest | Via folly |
| c-ares | getdeps manifest | Via proxygen |
| liboqs | getdeps manifest | Via fizz (post-quantum crypto) |

---

## Version History

- **Std-mode relay client fixes** (2026-02-16): Multiple fixes for picoquic relay client flow:
  - Fixed ALPN bug in `PicoquicMoQClient.cpp`: `picoquic_create_cnx()` was using empty `config_.alpn` instead of the computed ALPN value. Added `computedAlpn_` member variable.
  - Added `moq://` URL scheme support in `PicoDateServer.cpp` URL parser.
  - Fixed transport mode selection based on URL scheme: `moq://` → raw QUIC, `https://` → WebTransport.
  - Added session factory support to `PicoquicMoQClient` for creating `MoQRelaySession` instead of `MoQSession`.
  - Fixed `PicoquicMoQRelayClient` to use `MoQRelaySession::createRelaySessionFactory()`.
  - **Critical fix**: Added `flushControlBuf()` call after `writePublishNamespace()` and `writeSubscribeNamespace()` in `MoQRelaySessionCompat.cpp` - control messages were being written but never sent.

- **MoQDateServer minute boundary fix** (2026-02-14): Fixed "Object ID not advancing"
  error when crossing minute boundaries. The `publishDate()` function now properly
  tracks group changes and ends the previous group before starting a new subgroup.
  (commit c1b5c59)
- **Current:** Supports MoQ draft versions (see `MoQVersions.h`)
- Transport abstraction allows mvfst or picoquic backends
- Std-mode support for environments without Folly
- **Multi-mode transport support** (2026-02-09): Added `--transport` flag to picoquic samples
  supporting both raw QUIC (`moq-00` ALPN) and WebTransport (`h3` ALPN). Enables cross-flavor
  interoperability: picoquic clients can connect to mvfst relays and vice versa. New files:
  `TransportMode.h`, `PicoquicH3Transport.h/cpp`.
- **Multi-mode test framework** (2026-02-09): Added 64 unit tests + 11 integration tests
  for std-mode + picoquic builds. Tests include framer/codec tests (18), test framework
  validation (20), callback-based session tests (26), and picoquic integration tests (11).
- **MoQSessionCompat fully implemented** (2026-02-04): subscribe, fetch, data delivery
  (subgroups, object streams, datagrams), subscribe done, unsubscribe, and all control
  message handlers. Picoquic sample programs (picodateserver, picotextclient) can
  exchange data end-to-end.
