#!/usr/bin/env bash
#
# xlog_category_smoke.sh — prove per-layer folly XLOG category filtering works.
#
# Regression guard for the category rooting in this PR: the -fmacro-prefix-map
# dep maps in standalone/CMakeLists.txt (folly/fizz/wangle/quic.mvfst/proxygen/
# moxygen) and the XLOG_SET_CATEGORY_NAME("quic.picoquic") in PicoQuicXLogSink.
# If any of those regress, that layer's category reverts to the absolute build
# path (home.runner.work...) or the file-derived default, and its per-layer
# `--logging=<layer>=...` selector silently matches nothing while the root level
# keeps working.
#
# This is a behavioral smoke test, not a unit test: the category is a
# compile-time property of each source file's __FILE__ (or, for picoquic, an
# explicit override), and most of these sources are not ours, so the faithful
# check is to run the built binaries and observe which layer a selector enables.
#
# It drives real sessions (a client-less server never does a TLS/QUIC handshake,
# so fizz/mvfst/picoquic would never emit) and captures the server's stderr:
#   - an mvfst session (moqdateserver <-> moqtextclient) for the six deps that
#     ride the default transport;
#   - a picoquic session (pico_evb_relay_server <-> pico_evb_text_client) for
#     quic.picoquic, if those binaries were built (BUILD_PICOQUIC) and openssl
#     is available to mint a throwaway cert.
#
# Only layers actually built on the folly XLOG backend emit XLOG at all; layers
# still on glog (or not exercised by a given session) are SKIPPED, never failed.
#
# Usage: xlog_category_smoke.sh <moqdateserver> <moqtextclient> \
#            [pico_evb_relay_server] [pico_evb_text_client] [base-port]

set -uo pipefail

DS="${1:?usage: <moqdateserver> <moqtextclient> [pico_relay] [pico_client] [port]}"
TC="${2:?usage: <moqdateserver> <moqtextclient> [pico_relay] [pico_client] [port]}"
PICO_SRV="${3:-}"
PICO_CLI="${4:-}"
PORT="${5:-14337}"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# Match only DBG-level lines (folly's GlogStyleFormatter prefixes them 'V';
# INFO/WARN/ERR are I/W/E and appear regardless of the selector, so matching
# those would give a false pass). $1 = file-marker regex; log on stdin.
has_dbg() { grep -Eq "^V[0-9].*(${1})"; }

fail=0

# ─────────────────────────────────────────────────────────────────────────────
# assert_scoping: given a session runner and a layer->marker map, verify each
# active layer's selector enables its own DBG lines and excludes the others.
#   $1 = name of a bash function: run <logging-spec> -> prints server stderr
#   $2 = name of an associative array (layer -> DBG-line file-marker regex)
# ─────────────────────────────────────────────────────────────────────────────
assert_scoping() {
  local runner="$1"; local -n MARK="$2"
  local -a layers=("${!MARK[@]}") active=()

  # Sanity: at the root, a layer's marker must appear or that layer is not on
  # the XLOG backend / not exercised by this session — skip it, do not fail.
  local root_out; root_out="$($runner 'DBG9')"
  local L
  for L in "${layers[@]}"; do
    if has_dbg "${MARK[$L]}" <<<"$root_out"; then
      active+=("$L")
    else
      echo "SKIP  $L: marker /${MARK[$L]}/ not emitted at root DBG9 (glog backend, not exercised, or stale marker)"
    fi
  done

  # DISCOVERY: dump the DBG-level (V) source files this session actually emits at
  # root DBG9, so markers are chosen from reality instead of guessed. Only V-level
  # files are targetable by a per-layer selector.
  echo "== INVENTORY[$runner] DBG-level files at root DBG9 =="
  grep -E '^V[0-9]' <<<"$root_out" \
    | grep -oE '[A-Za-z_][A-Za-z_0-9]*\.(cpp|h):[0-9]+' | sed -E 's/:[0-9]+$//' \
    | sort | uniq -c | sort -rn | sed 's/^/    /'

  # One run per active layer: assert its own marker is present AND every other
  # active layer's marker is absent (that exclusion is the proof of scoping, and
  # catches category collisions like a bare quic.* shared by mvfst and picoquic).
  local O out
  for L in "${active[@]}"; do
    out="$($runner "$L=DBG9")"
    if has_dbg "${MARK[$L]}" <<<"$out"; then
      echo "PASS  $L=DBG9 selects /${MARK[$L]}/"
    else
      echo "FAIL  $L=DBG9 did NOT select /${MARK[$L]}/ (category not rooted at '$L')"
      fail=1
    fi
    for O in "${active[@]}"; do
      [ "$O" = "$L" ] && continue
      if has_dbg "${MARK[$O]}" <<<"$out"; then
        echo "FAIL  $L=DBG9 leaked /${MARK[$O]}/ ($O not scoped out of '$L')"
        fail=1
      fi
    done
  done
  [ "${#active[@]}" -gt 0 ] || echo "WARN  no layers active for $runner (nothing verified)"
}

# ── mvfst session: the six prefix-map layers on the default transport ─────────
run_mvfst() {
  local spec="$1" log="$WORK/mvfst.log"
  "$DS" --insecure -port "$PORT" --logging="$spec" >/dev/null 2>"$log" &
  local sp=$!
  sleep 1
  timeout 3 "$TC" --insecure \
    --connect_url "https://localhost:$PORT/moq-date" \
    --track_namespace "moq-date" --track_name "date" \
    --logging=INFO >/dev/null 2>/dev/null
  sleep 0.3
  kill "$sp" 2>/dev/null; wait "$sp" 2>/dev/null
  cat "$log"
}

declare -A MVFST_MARK=(
  [moxygen]='MoQSession\.cpp|MoQForwarder\.cpp'
  [fizz]='AeadTokenCipher\.cpp|FizzServer|Fizz.*\.cpp'
  [quic.mvfst]='QuicServer\.cpp|QuicTransport'
  [wangle]='Acceptor\.cpp|ConnectionManager\.cpp'
  [proxygen]='HQSession|HTTPTransaction|HQ.*Session'
  [folly]='AsyncSocket\.cpp|AsyncUDPSocket\.cpp|EventBase\.cpp'
)
echo "── mvfst transport ──"
assert_scoping run_mvfst MVFST_MARK

# ── picoquic session: quic.picoquic (guarded — needs pico binaries + openssl) ─
if [ -n "$PICO_SRV" ] && [ -x "$PICO_SRV" ] && [ -n "$PICO_CLI" ] && [ -x "$PICO_CLI" ] \
   && command -v openssl >/dev/null 2>&1; then
  PPORT=$((PORT + 1))
  openssl req -x509 -newkey rsa:2048 -nodes -keyout "$WORK/key.pem" \
    -out "$WORK/cert.pem" -days 1 -subj "/CN=localhost" \
    -addext "subjectAltName=DNS:localhost" >/dev/null 2>&1

  run_pico() {
    local spec="$1" log="$WORK/pico.log"
    "$PICO_SRV" -cert "$WORK/cert.pem" -key "$WORK/key.pem" \
      -port "$PPORT" -endpoint "/moq-relay" --logging="$spec" >/dev/null 2>"$log" &
    local sp=$!
    sleep 1
    timeout 3 "$PICO_CLI" \
      --connect_url "moqt://localhost:$PPORT/moq-relay" \
      --track_namespace "moq-date" --track_name "date" \
      --cert_root "$WORK/cert.pem" --logging=INFO >/dev/null 2>/dev/null
    sleep 0.3
    kill "$sp" 2>/dev/null; wait "$sp" 2>/dev/null
    cat "$log"
  }

  # quic.picoquic is set via XLOG_SET_CATEGORY_NAME, so the strongest proof is
  # that quic.picoquic= selects the sink's lines while the old file-derived
  # category (openmoq.transport.pico) does NOT — that exclusion only holds if
  # the override took effect. Marker is the sink's __FILE__ (shown by glog fmt).
  echo "── picoquic transport ──"
  root_pico="$(run_pico 'DBG9')"
  echo "== INVENTORY[pico] all source files at root DBG9 (any output ⇒ handshake happened) =="
  grep -oE '[A-Za-z_][A-Za-z_0-9]*\.(c|cpp|h):[0-9]+' <<<"$root_pico" | sed -E 's/:[0-9]+$//' \
    | sort | uniq -c | sort -rn | sed 's/^/    /'
  pico_lines=$(printf '%s\n' "$root_pico" | wc -l)
  echo "    (total pico server stderr lines: $pico_lines)"
  if has_dbg 'PicoQuicXLogSink\.cpp' <<<"$root_pico"; then
    if has_dbg 'PicoQuicXLogSink\.cpp' <<<"$(run_pico 'quic.picoquic=DBG9')"; then
      echo "PASS  quic.picoquic=DBG9 selects /PicoQuicXLogSink.cpp/"
    else
      echo "FAIL  quic.picoquic=DBG9 did NOT select the sink (XLOG_SET_CATEGORY_NAME not effective?)"
      fail=1
    fi
    if has_dbg 'PicoQuicXLogSink\.cpp' <<<"$(run_pico 'openmoq.transport.pico=DBG9')"; then
      echo "FAIL  openmoq.transport.pico=DBG9 still selects the sink (override NOT applied)"
      fail=1
    else
      echo "PASS  openmoq.transport.pico=DBG9 excludes the sink (override applied → quic.picoquic)"
    fi
  else
    # Expected today: installPicoQuicXLogSink() has no callers, so the sink
    # never emits. Auto-activates once that is wired — tracked in #318.
    echo "SKIP  quic.picoquic: sink emitted no DBG at root — installer unwired (openmoq/moxygen#318)"
  fi
else
  echo "SKIP  quic.picoquic: pico sample binaries not built (BUILD_PICOQUIC off) or openssl absent"
fi

[ "$fail" -eq 0 ] && echo "OK: per-layer XLOG category filtering verified"

# >>> DISCOVERY ROUND (temporary): force a non-zero exit so ctest's
# --output-on-failure surfaces the INVENTORY dumps above in the CI log. Revert
# this block (restore `exit "$fail"`) once the wangle/proxygen/folly markers are
# corrected from the observed inventory.
echo ">>> DISCOVERY ROUND: forcing failure to surface inventory (temporary) <<<"
exit 1
