#!/usr/bin/env bash
#
# xlog_category_smoke.sh — prove per-layer folly XLOG category filtering works.
#
# Regression guard for the -fmacro-prefix-map dependency-category rooting in
# standalone/CMakeLists.txt. Each bundled dependency (fizz, mvfst, moxygen, ...)
# must compile so that folly derives a category rooted at its natural name
# (fizz.*, quic.mvfst.*, moxygen.*, ...). If those maps are dropped or a dep's
# layout shifts, categories silently revert to the absolute build path
# (home.runner.work...) and every per-layer `--logging=<layer>=...` selector
# matches nothing — while the root level keeps working, so nothing else notices.
#
# This is a behavioral smoke test, not a unit test: the category is a
# compile-time property of each dependency's __FILE__, and those sources are not
# ours, so the only faithful check is to run the built binaries and observe
# which layer's lines a selector actually enables.
#
# It drives a short date-server <-> text-client session (a client-less server
# never runs a TLS/QUIC handshake, so fizz/mvfst would never emit). The server's
# stderr is captured. Only layers actually built on the folly XLOG backend emit
# XLOG at all; layers still on glog (or not exercised) are SKIPPED, never failed,
# so the test stays honest across pin advances.
#
# Usage: xlog_category_smoke.sh <moqdateserver> <moqtextclient> [port]

set -uo pipefail

DS="${1:?usage: xlog_category_smoke.sh <moqdateserver> <moqtextclient> [port]}"
TC="${2:?usage: xlog_category_smoke.sh <moqdateserver> <moqtextclient> [port]}"
PORT="${3:-14337}"

# layer (folly category root) -> a source-file marker that layer emits at DBG9,
# server-side, during a subscribe session. Update the marker (not the category)
# if a log line is removed upstream.
LAYERS=(moxygen fizz quic.mvfst)
declare -A MARKER=(
  [moxygen]='MoQSession\.cpp|MoQForwarder\.cpp'
  [fizz]='AeadTokenCipher\.cpp|FizzServer|Fizz.*\.cpp'
  [quic.mvfst]='QuicServer\.cpp|QuicTransport'
)

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# Match only DBG-level lines (folly's GlogStyleFormatter prefixes them 'V';
# INFO/WARN/ERR are I/W/E and appear regardless of the selector, so matching
# those would give a false pass). $1 = file-marker regex; log on stdin.
has_dbg() { grep -Eq "^V[0-9].*(${1})"; }

# run <server-logging-spec> -> prints the server's captured stderr for one
# server-up / client-subscribe / server-down cycle.
run() {
  local spec="$1" log="$WORK/srv.log"
  "$DS" --insecure -port "$PORT" --logging="$spec" >/dev/null 2>"$log" &
  local sp=$!
  sleep 1
  timeout 3 "$TC" --insecure \
    --connect_url "https://localhost:$PORT/moq-date" \
    --track_namespace "moq-date" --track_name "date" \
    --logging=INFO >/dev/null 2>/dev/null
  sleep 0.3
  kill "$sp" 2>/dev/null
  wait "$sp" 2>/dev/null
  cat "$log"
}

fail=0
active=()

# Sanity: at the root every marker must appear, else that layer is not on the
# XLOG backend in this build (or the marker is stale). Skip it — do not fail.
root_out="$(run 'DBG9')"
for L in "${LAYERS[@]}"; do
  if has_dbg "${MARKER[$L]}" <<<"$root_out"; then
    active+=("$L")
  else
    echo "SKIP  $L: marker /${MARKER[$L]}/ not emitted at root DBG9 (glog backend, or stale marker)"
  fi
done

# For each active layer: its own selector must enable it, and every OTHER active
# layer's selector must NOT enable it (this exclusion is the proof of scoping;
# the old absolute-path bug fails the 'own selector' half).
for L in "${active[@]}"; do
  if has_dbg "${MARKER[$L]}" <<<"$(run "$L=DBG9")"; then
    echo "PASS  $L=DBG9 selects /${MARKER[$L]}/"
  else
    echo "FAIL  $L=DBG9 did NOT select /${MARKER[$L]}/ (category not rooted at '$L' — maps dropped?)"
    fail=1
  fi
  for O in "${active[@]}"; do
    [ "$O" = "$L" ] && continue
    if has_dbg "${MARKER[$L]}" <<<"$(run "$O=DBG9")"; then
      echo "FAIL  $O=DBG9 leaked /${MARKER[$L]}/ ($L not scoped under '$O')"
      fail=1
    fi
  done
done

if [ "${#active[@]}" -eq 0 ]; then
  echo "ERROR no layer emitted XLOG at root DBG9 — check the XLOG backend is enabled"
  exit 2
fi

[ "$fail" -eq 0 ] && echo "OK: per-layer XLOG category filtering verified (${active[*]})"
exit "$fail"
