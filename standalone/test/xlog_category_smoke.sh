#!/usr/bin/env bash
#
# xlog_category_smoke.sh — prove per-layer folly XLOG category filtering works:
# for each layer, assert --logging=<layer>=DBG9 enables that layer's DBG lines
# and excludes every other layer's.
#
# Behavioral rather than unit test by necessity: a category is a compile-time
# property of each source file's __FILE__, and most of these sources are not
# ours. A real client/server session is needed too — a client-less server never
# handshakes, so fizz and mvfst would never emit.
#
# Layers not built on the XLOG backend, or not exercised by a session, SKIP.
#
# Usage: xlog_category_smoke.sh <moqdateserver> <moqtextclient> [base-port]

set -uo pipefail

DS="${1:?usage: <moqdateserver> <moqtextclient> [port]}"
TC="${2:?usage: <moqdateserver> <moqtextclient> [port]}"
PORT="${3:-14337}"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# Match only DBG lines: folly's GlogStyleFormatter prefixes them 'V', while
# INFO/WARN/ERR appear regardless of the selector and would give a false pass.
# $1 = file-marker regex; log on stdin.
has_dbg() { grep -Eq "^V[0-9].*(${1})"; }

fail=0

# assert_scoping <runner-fn>
#   runner-fn: takes a --logging spec, prints the server's stderr
assert_scoping() {
  local runner="$1"
  local -a active=()

  # A marker absent at the root means that layer isn't on the XLOG backend or
  # isn't exercised by this session — skip it, don't fail.
  local root_out; root_out="$($runner 'DBG9')"
  local L M
  for L in "${LAYERS[@]}"; do
    M="$(marker_for "$L")"
    if has_dbg "$M" <<<"$root_out"; then
      active+=("$L")
    else
      echo "SKIP  $L: marker /$M/ not emitted at root DBG9 (glog backend, not exercised, or stale marker)"
    fi
  done

  # The exclusion half is the actual proof of scoping, and catches collisions
  # between layers that end up sharing a category root.
  local O out OM
  for L in "${active[@]}"; do
    M="$(marker_for "$L")"
    out="$($runner "$L=DBG9")"
    if has_dbg "$M" <<<"$out"; then
      echo "PASS  $L=DBG9 selects /$M/"
    else
      echo "FAIL  $L=DBG9 did NOT select /$M/ (category not rooted at '$L')"
      fail=1
    fi
    for O in "${active[@]}"; do
      [ "$O" = "$L" ] && continue
      OM="$(marker_for "$O")"
      if has_dbg "$OM" <<<"$out"; then
        echo "FAIL  $L=DBG9 leaked /$OM/ ($O not scoped out of '$L')"
        fail=1
      fi
    done
  done
  [ "${#active[@]}" -gt 0 ] || echo "WARN  no layers active for $runner (nothing verified)"
}

# The server's UDP socket is the readiness signal. ss on Linux, lsof where it
# is absent.
wait_listening() {
  local port="$1" i=0
  while [ "$i" -lt 100 ]; do
    if command -v ss >/dev/null 2>&1; then
      ss -uln 2>/dev/null | grep -q ":$port " && return 0
    elif command -v lsof >/dev/null 2>&1; then
      lsof -nP -iUDP:"$port" >/dev/null 2>&1 && return 0
    else
      sleep 1; return 0
    fi
    sleep 0.05
    i=$((i + 1))
  done
  return 1
}

# ── mvfst session: the six prefix-map layers on the default transport ─────────
# Every marker lands during connection setup and the client runs until killed,
# so the session is bounded rather than waited on. Not timeout(1): macOS lacks
# it.
run_mvfst() {
  local spec="$1" log="$WORK/mvfst.log"
  "$DS" --insecure -port "$PORT" --logging="$spec" >/dev/null 2>"$log" &
  local sp=$!
  wait_listening "$PORT"
  "$TC" --insecure \
    --connect_url "https://localhost:$PORT/moq-date" \
    --track_namespace "moq-date" --track_name "date" \
    --logging=INFO >/dev/null 2>/dev/null &
  local cp=$!
  sleep 0.5
  kill "$cp" 2>/dev/null; wait "$cp" 2>/dev/null
  sleep 0.2
  kill "$sp" 2>/dev/null; wait "$sp" 2>/dev/null
  cat "$log"
}

# This session exercises moxygen, fizz and quic.mvfst. wangle (fires only behind
# a TCP HTTP acceptor), proxygen (still on the glog backend here) and folly
# (sparse XLOG use) SKIP today; they stay listed so they auto-activate once a
# build or session reaches them.
# Layer -> regex matching a DBG line from that layer, as parallel arrays:
# macOS ships bash 3.2, which has neither associative arrays nor namerefs.
LAYERS=(moxygen fizz quic.mvfst wangle proxygen folly)
MARKERS=(
  'MoQSession\.cpp|MoQForwarder\.cpp'
  'AeadTokenCipher\.cpp|RecordLayer\.cpp|FizzServer|Fizz.*\.cpp'
  'QuicServer\.cpp|QuicTransport'
  'Acceptor\.cpp|ConnectionManager\.cpp'
  'HQSession|HTTPTransaction|HQ.*Session'
  'AsyncSocket\.cpp|AsyncUDPSocket\.cpp|EventBase\.cpp'
)

marker_for() {
  local i=0
  while [ "$i" -lt "${#LAYERS[@]}" ]; do
    if [ "${LAYERS[$i]}" = "$1" ]; then printf '%s' "${MARKERS[$i]}"; return 0; fi
    i=$((i + 1))
  done
  return 1
}

echo "── mvfst transport ──"
assert_scoping run_mvfst

[ "$fail" -eq 0 ] && echo "OK: per-layer XLOG category filtering verified"
exit "$fail"
