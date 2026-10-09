#!/usr/bin/env bash
# pgo-train.sh — Train a GCC PGO profile by driving the sample relay over loopback.
#
# Runs moqrelayserver from an instrumented install prefix under a moqtest_server
# publisher and moqperf_test_client subscriber load, one scenario at a time, and
# stops the relay with SIGTERM so libgcov writes its counters on exit. Only the
# relay's counters are kept: the publisher and client dump under a discard
# prefix (GCOV_PREFIX). Scenarios share one profile directory, so libgcov sums
# their counters.
#
# Usage:
#   pgo-train.sh --prefix DIR --profile-dir DIR [--work-dir DIR]
#                [--duration SECONDS] [--subscribers N] [--port PORT]
#
# Fails when a scenario fails, when the relay does not exit on SIGTERM (its
# counters would be lost), or when the profile lacks the relay's hot objects.

set -euo pipefail

PREFIX=""
PROFILE_DIR=""
WORK_DIR=""
DURATION=60
SUBSCRIBERS=100
PORT=4443
PUB_PORT=4499

# Columns: name, --quic_transport, duration percent, first/other object bytes,
# objects per group. 26516/3788 x 60 is the nightly perf-test object mix.
SCENARIOS=(
  "quic-nightly-mix  true   100  26516  3788  60"
  "wt-nightly-mix    false  100  26516  3788  60"
  "quic-small-objs   true   50   7576   1894  30"
)

# Objects whose counters prove the relay's hot paths ran: moxygen session,
# mvfst send path, folly event loop.
HOT_OBJECTS=(MoQSession.cpp QuicTransportFunctions.cpp EventBase.cpp)

usage() {
  cat <<USAGE
Usage: $(basename "$0") --prefix DIR --profile-dir DIR [options]

  --prefix DIR        Instrumented install prefix (bin/moqrelayserver, moqtest_server,
                      moqperf_test_client)
  --profile-dir DIR   Directory the instrumented build writes .gcda files to
                      (-fprofile-generate=DIR)
  --work-dir DIR      Logs and discarded counters (default: mktemp)
  --duration SECONDS  Base duration per scenario (default: $DURATION)
  --subscribers N     Peak subscribers per scenario (default: $SUBSCRIBERS)
  --port PORT         Relay UDP port (default: $PORT)
  -h, --help          Show this help
USAGE
  exit "${1:-0}"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --prefix)       PREFIX="$2"; shift 2 ;;
    --profile-dir)  PROFILE_DIR="$2"; shift 2 ;;
    --work-dir)     WORK_DIR="$2"; shift 2 ;;
    --duration)     DURATION="$2"; shift 2 ;;
    --subscribers)  SUBSCRIBERS="$2"; shift 2 ;;
    --port)         PORT="$2"; shift 2 ;;
    -h|--help)      usage 0 ;;
    *) echo "Unknown option: $1" >&2; usage 1 ;;
  esac
done

log() { printf '[pgo-train] %s\n' "$*"; }
die() { log "ERROR: $*" >&2; exit 1; }

[[ -n "$PREFIX" && -n "$PROFILE_DIR" ]] || usage 1
[[ -d "$PROFILE_DIR" ]] || die "profile dir $PROFILE_DIR does not exist"
for bin in moqrelayserver moqtest_server moqperf_test_client; do
  [[ -x "$PREFIX/bin/$bin" ]] || die "$PREFIX/bin/$bin missing or not executable"
done
command -v ss >/dev/null || die "ss (iproute2) is required"
[[ -n "$WORK_DIR" ]] || WORK_DIR="$(mktemp -d)"
mkdir -p "$WORK_DIR"
DISCARD="$WORK_DIR/discard"
RELAY_URL="https://127.0.0.1:${PORT}/moq-relay"
RAMP=$(( SUBSCRIBERS / 4 ))
(( RAMP >= 1 )) || RAMP=1

RELAY_PID=""
PUB_PID=""
cleanup() {
  if [[ -n "$PUB_PID" ]]; then kill "$PUB_PID" 2>/dev/null || true; fi
  if [[ -n "$RELAY_PID" ]]; then kill "$RELAY_PID" 2>/dev/null || true; fi
}
trap cleanup EXIT

wait_for_udp_port() {
  local port=$1 deadline=$(( SECONDS + $2 ))
  until ss -lun 2>/dev/null | grep -qE ":${port}[[:space:]]"; do
    (( SECONDS < deadline )) || return 1
    sleep 0.5
  done
}

# SIGTERM, then wait: the relay returns from main and libgcov writes its
# counters on exit. A relay that has to be killed leaves nothing behind.
stop_relay() {
  local deadline=$(( SECONDS + 60 ))
  kill -TERM "$RELAY_PID"
  while kill -0 "$RELAY_PID" 2>/dev/null; do
    if (( SECONDS >= deadline )); then
      kill -KILL "$RELAY_PID" 2>/dev/null || true
      die "relay did not exit within 60s of SIGTERM; counters not written"
    fi
    sleep 0.5
  done
  local rc=0
  wait "$RELAY_PID" || rc=$?
  RELAY_PID=""
  (( rc == 0 )) || die "relay exited with status $rc"
}

run_scenario() {
  local name=$1 qt=$2 pct=$3 first=$4 other=$5 per_group=$6
  local dur=$(( DURATION * pct / 100 ))
  (( dur >= 10 )) || dur=10
  local dir="$WORK_DIR/$name"
  mkdir -p "$dir"
  log "scenario $name: quic_transport=$qt subscribers=$SUBSCRIBERS ramp=$RAMP/s duration=${dur}s objects=${first}/${other} x ${per_group}"

  "$PREFIX/bin/moqrelayserver" --insecure --port "$PORT" >"$dir/relay.log" 2>&1 &
  RELAY_PID=$!
  wait_for_udp_port "$PORT" 15 || die "relay not listening on udp/$PORT (see $dir/relay.log)"

  GCOV_PREFIX="$DISCARD" "$PREFIX/bin/moqtest_server" \
    --relay_url="$RELAY_URL" --quic_transport="$qt" \
    --include_timestamp_extension=true --port "$PUB_PORT" \
    >"$dir/publisher.log" 2>&1 &
  PUB_PID=$!
  sleep 2
  kill -0 "$PUB_PID" 2>/dev/null || die "publisher exited early (see $dir/publisher.log)"

  local client_rc=0
  GCOV_PREFIX="$DISCARD" timeout $(( dur + 60 )) "$PREFIX/bin/moqperf_test_client" \
    --relay_url="$RELAY_URL" --quic_transport="$qt" \
    --subscriber_max="$SUBSCRIBERS" --subscriber_ramp="$RAMP" \
    --duration="$dur" --delivery_timeout=5000 --num_threads=2 \
    --first_object_size="$first" --other_object_size="$other" \
    --objects_per_group="$per_group" \
    >"$dir/client.log" 2>&1 || client_rc=$?

  kill -TERM "$PUB_PID" 2>/dev/null || true
  stop_relay
  wait "$PUB_PID" 2>/dev/null || true
  PUB_PID=""

  if (( client_rc != 0 )); then
    tail -n 20 "$dir/client.log" >&2
    die "scenario $name: client exited with status $client_rc"
  fi
  grep -F '[AGGREGATE]' "$dir/client.log" | tail -n 1 || true
  grep -qE 'Total Objects: *[1-9]' "$dir/client.log" \
    || die "scenario $name: client reports no objects received (see $dir/client.log)"
}

for row in "${SCENARIOS[@]}"; do
  # shellcheck disable=SC2086
  run_scenario $row
done

count=$(find "$PROFILE_DIR" -name '*.gcda' | wc -l)
(( count > 0 )) || die "no .gcda files under $PROFILE_DIR"
for obj in "${HOT_OBJECTS[@]}"; do
  find "$PROFILE_DIR" -name "*${obj}.gcda" -size +0 | grep -q . \
    || die "no counters for $obj under $PROFILE_DIR"
done
log "profile: $count .gcda files, $(du -sh "$PROFILE_DIR" | cut -f1), hot objects present: ${HOT_OBJECTS[*]}"
