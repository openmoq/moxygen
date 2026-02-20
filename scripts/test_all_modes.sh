#!/bin/bash
#
# test_all_modes.sh - Build and test all moxygen build modes
#
# Usage: ./scripts/test_all_modes.sh [--skip-build] [--mode1-only] [--mode2-only]
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_DIR"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Parse arguments
SKIP_BUILD=false
MODE1_ONLY=false
MODE2_ONLY=false

for arg in "$@"; do
  case $arg in
    --skip-build) SKIP_BUILD=true ;;
    --mode1-only) MODE1_ONLY=true ;;
    --mode2-only) MODE2_ONLY=true ;;
    --help|-h)
      echo "Usage: $0 [--skip-build] [--mode1-only] [--mode2-only]"
      echo "  --skip-build  Skip building, only run tests"
      echo "  --mode1-only  Only test Mode 1 (Folly + mvfst)"
      echo "  --mode2-only  Only test Mode 2 (std + picoquic)"
      exit 0
      ;;
  esac
done

# Results tracking
declare -a TEST_RESULTS
PASS_COUNT=0
FAIL_COUNT=0

log_info() {
  echo -e "${YELLOW}[INFO]${NC} $1"
}

log_pass() {
  echo -e "${GREEN}[PASS]${NC} $1"
  TEST_RESULTS+=("PASS: $1")
  ((PASS_COUNT++))
}

log_fail() {
  echo -e "${RED}[FAIL]${NC} $1"
  TEST_RESULTS+=("FAIL: $1")
  ((FAIL_COUNT++))
}

cleanup() {
  log_info "Cleaning up processes..."
  pkill -f moqrelayserver 2>/dev/null || true
  pkill -f moqdateserver 2>/dev/null || true
  pkill -f moqtextclient 2>/dev/null || true
  pkill -f picodateserver 2>/dev/null || true
  pkill -f picotextclient 2>/dev/null || true
  pkill -f picorelayserver 2>/dev/null || true
  sleep 1
}

# Ensure cleanup on exit
trap cleanup EXIT

# Generate TLS certs if needed
generate_certs() {
  if [ ! -f cert.pem ] || [ ! -f key.pem ]; then
    log_info "Generating TLS certificates..."
    openssl genrsa -out key.pem 2048 2>/dev/null
    openssl req -new -x509 -key key.pem -out cert.pem -days 365 -subj "/CN=localhost" 2>/dev/null
  fi
}

# ============================================================================
# BUILD FUNCTIONS
# ============================================================================

build_mode1() {
  log_info "Building Mode 1 (Folly + mvfst)..."

  # Copy cached downloads if available
  if [ -d "_build_folly_pico/downloads" ]; then
    mkdir -p _build/downloads
    cp _build_folly_pico/downloads/* _build/downloads/ 2>/dev/null || true
  fi

  ./build/fbcode_builder/getdeps.py build --src-dir=moxygen:. moxygen \
    --allow-system-packages \
    --scratch-path _build \
    --extra-cmake-defines '{"CMAKE_POLICY_VERSION_MINIMUM":"3.5"}'

  # Fix dylib paths (macOS)
  mkdir -p _build/deps/lib
  find "$(pwd)/_build/installed" -name "*.dylib" -exec ln -sf {} _build/deps/lib/ \;

  log_info "Mode 1 build complete"
}

build_mode2() {
  log_info "Building Mode 2 (std + picoquic)..."

  cmake -S . -B _build_std \
    -DMOXYGEN_USE_FOLLY=OFF \
    -DMOXYGEN_QUIC_BACKEND=picoquic \
    -DMOXYGEN_TLS_BACKEND=openssl \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo

  cmake --build _build_std --parallel

  log_info "Mode 2 build complete"
}

# ============================================================================
# TEST HELPER FUNCTIONS
# ============================================================================

wait_for_server() {
  local pid=$1
  local timeout=${2:-5}
  local count=0
  while [ $count -lt $timeout ]; do
    if ps -p $pid > /dev/null 2>&1; then
      sleep 1
      ((count++))
    else
      return 1
    fi
  done
  return 0
}

run_client_test() {
  local client_cmd="$1"
  local test_name="$2"
  local timeout=${3:-6}

  local output_file="/tmp/client_test_$$.log"

  # Run client in background
  eval "$client_cmd" > "$output_file" 2>&1 &
  local client_pid=$!

  # Wait for timeout then kill
  sleep $timeout
  kill $client_pid 2>/dev/null || true
  wait $client_pid 2>/dev/null || true

  # Check results
  if grep -qE "Connected|Largest=" "$output_file" && grep -qE "^[0-9]+$" "$output_file"; then
    log_pass "$test_name"
    return 0
  else
    log_fail "$test_name"
    echo "--- Output ---"
    tail -20 "$output_file"
    return 1
  fi
}

# ============================================================================
# MODE 2 TESTS (std + picoquic)
# ============================================================================

test_mode2_raw_quic_direct() {
  log_info "Testing Mode 2: Raw QUIC (Direct)..."
  cleanup

  ./_build_std/moxygen/samples/date/picodateserver \
    --cert cert.pem --key key.pem \
    --port 4433 --ns moq-date --mode spg > /tmp/server.log 2>&1 &
  local server_pid=$!
  sleep 3

  run_client_test \
    "./_build_std/moxygen/samples/text-client/picotextclient --host localhost --port 4433 --ns moq-date --track date --insecure" \
    "Mode 2: Raw QUIC Direct"

  kill $server_pid 2>/dev/null || true
}

test_mode2_webtransport_direct() {
  log_info "Testing Mode 2: WebTransport (Direct)..."
  cleanup

  ./_build_std/moxygen/samples/date/picodateserver \
    --cert cert.pem --key key.pem \
    --port 4433 --ns moq-date --mode spg \
    --transport webtransport > /tmp/server.log 2>&1 &
  local server_pid=$!
  sleep 3

  run_client_test \
    "./_build_std/moxygen/samples/text-client/picotextclient --host localhost --port 4433 --ns moq-date --track date --insecure --transport webtransport" \
    "Mode 2: WebTransport Direct"

  kill $server_pid 2>/dev/null || true
}

# ============================================================================
# MODE 1 TESTS (Folly + mvfst)
# ============================================================================

test_mode1_raw_quic_relay() {
  log_info "Testing Mode 1: Raw QUIC via Relay..."
  cleanup

  export PATH="$(pwd)/_build/installed/moxygen/bin:$PATH"
  export DYLD_LIBRARY_PATH="$(pwd)/_build/deps/lib:$DYLD_LIBRARY_PATH"

  moqrelayserver --insecure --port 4433 > /tmp/relay.log 2>&1 &
  local relay_pid=$!
  sleep 2

  moqdateserver --insecure --port 4435 --quic_transport \
    --relay_url "https://localhost:4433/moq-relay" \
    --ns "moq-date" > /tmp/dateserver.log 2>&1 &
  local date_pid=$!
  sleep 3

  run_client_test \
    "moqtextclient --insecure --quic_transport --connect_url 'https://localhost:4433/moq-relay' --track_namespace 'moq-date' --track_name 'date'" \
    "Mode 1: Raw QUIC via Relay"

  kill $date_pid 2>/dev/null || true
  kill $relay_pid 2>/dev/null || true
}

test_mode1_webtransport_relay() {
  log_info "Testing Mode 1: WebTransport via Relay..."
  cleanup

  export PATH="$(pwd)/_build/installed/moxygen/bin:$PATH"
  export DYLD_LIBRARY_PATH="$(pwd)/_build/deps/lib:$DYLD_LIBRARY_PATH"

  moqrelayserver --insecure --port 4433 > /tmp/relay.log 2>&1 &
  local relay_pid=$!
  sleep 2

  moqdateserver --insecure --port 4435 \
    --relay_url "https://localhost:4433/moq-relay" \
    --ns "moq-date" > /tmp/dateserver.log 2>&1 &
  local date_pid=$!
  sleep 3

  run_client_test \
    "moqtextclient --insecure --connect_url 'https://localhost:4433/moq-relay' --track_namespace 'moq-date' --track_name 'date'" \
    "Mode 1: WebTransport via Relay"

  kill $date_pid 2>/dev/null || true
  kill $relay_pid 2>/dev/null || true
}

test_mode1_direct() {
  log_info "Testing Mode 1: Direct Connection..."
  cleanup

  export PATH="$(pwd)/_build/installed/moxygen/bin:$PATH"
  export DYLD_LIBRARY_PATH="$(pwd)/_build/deps/lib:$DYLD_LIBRARY_PATH"

  moqdateserver --insecure --port 4433 --ns "moq-date" > /tmp/server.log 2>&1 &
  local server_pid=$!
  sleep 3

  run_client_test \
    "moqtextclient --insecure --connect_url 'https://localhost:4433/moq-date' --track_namespace 'moq-date' --track_name 'date'" \
    "Mode 1: Direct Connection"

  kill $server_pid 2>/dev/null || true
}

# ============================================================================
# CROSS-MODE TESTS
# ============================================================================

test_crossmode_pico_to_mvfst() {
  log_info "Testing Cross-mode: picoquic client → mvfst server..."
  cleanup

  export PATH="$(pwd)/_build/installed/moxygen/bin:$PATH"
  export DYLD_LIBRARY_PATH="$(pwd)/_build/deps/lib:$DYLD_LIBRARY_PATH"

  moqrelayserver --insecure --port 4433 > /tmp/relay.log 2>&1 &
  local relay_pid=$!
  sleep 2

  moqdateserver --insecure --port 4435 \
    --relay_url "https://localhost:4433/moq-relay" \
    --ns "moq-date" > /tmp/dateserver.log 2>&1 &
  local date_pid=$!
  sleep 3

  run_client_test \
    "./_build_std/moxygen/samples/text-client/picotextclient --host localhost --port 4433 --ns moq-date --track date --insecure --transport webtransport --path /moq-relay" \
    "Cross-mode: picoquic → mvfst (WebTransport)"

  kill $date_pid 2>/dev/null || true
  kill $relay_pid 2>/dev/null || true
}

test_crossmode_mvfst_to_pico() {
  log_info "Testing Cross-mode: mvfst client → picoquic server..."
  cleanup

  export PATH="$(pwd)/_build/installed/moxygen/bin:$PATH"
  export DYLD_LIBRARY_PATH="$(pwd)/_build/deps/lib:$DYLD_LIBRARY_PATH"

  ./_build_std/moxygen/samples/date/picodateserver \
    --cert cert.pem --key key.pem \
    --port 4433 --ns moq-date --mode spg \
    --transport webtransport > /tmp/server.log 2>&1 &
  local server_pid=$!
  sleep 3

  run_client_test \
    "moqtextclient --insecure --connect_url 'https://localhost:4433/moq' --track_namespace 'moq-date' --track_name 'date'" \
    "Cross-mode: mvfst → picoquic (WebTransport)"

  kill $server_pid 2>/dev/null || true
}

# ============================================================================
# MAIN
# ============================================================================

main() {
  echo "=============================================="
  echo "     Moxygen Build & Test Suite"
  echo "=============================================="
  echo ""

  generate_certs

  # Build phase
  if [ "$SKIP_BUILD" = false ]; then
    if [ "$MODE1_ONLY" = false ]; then
      build_mode2
    fi
    if [ "$MODE2_ONLY" = false ]; then
      if [ -f "_build/installed/moxygen/bin/moqrelayserver" ]; then
        log_info "Mode 1 build exists, skipping..."
      else
        build_mode1
      fi
    fi
  fi

  echo ""
  echo "=============================================="
  echo "     Running Tests"
  echo "=============================================="
  echo ""

  # Mode 2 tests
  if [ "$MODE1_ONLY" = false ]; then
    if [ -f "_build_std/moxygen/samples/date/picodateserver" ]; then
      test_mode2_raw_quic_direct
      test_mode2_webtransport_direct
    else
      log_fail "Mode 2 binaries not found"
    fi
  fi

  # Mode 1 tests
  if [ "$MODE2_ONLY" = false ]; then
    if [ -f "_build/installed/moxygen/bin/moqrelayserver" ]; then
      test_mode1_raw_quic_relay
      test_mode1_webtransport_relay
      test_mode1_direct
    else
      log_fail "Mode 1 binaries not found"
    fi
  fi

  # Cross-mode tests (require both modes)
  if [ "$MODE1_ONLY" = false ] && [ "$MODE2_ONLY" = false ]; then
    if [ -f "_build/installed/moxygen/bin/moqrelayserver" ] && \
       [ -f "_build_std/moxygen/samples/date/picodateserver" ]; then
      test_crossmode_pico_to_mvfst
      test_crossmode_mvfst_to_pico
    else
      log_info "Skipping cross-mode tests (need both Mode 1 and Mode 2 builds)"
    fi
  fi

  # Summary
  echo ""
  echo "=============================================="
  echo "     Test Results Summary"
  echo "=============================================="
  echo ""
  for result in "${TEST_RESULTS[@]}"; do
    if [[ $result == PASS* ]]; then
      echo -e "${GREEN}✓${NC} ${result#PASS: }"
    else
      echo -e "${RED}✗${NC} ${result#FAIL: }"
    fi
  done
  echo ""
  echo "Total: $PASS_COUNT passed, $FAIL_COUNT failed"
  echo ""

  if [ $FAIL_COUNT -gt 0 ]; then
    exit 1
  fi
}

main "$@"
