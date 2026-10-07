#!/usr/bin/env bash
#
# moqtest_client_flags.sh — check that moqtest_client still takes the options
# only openmoq has, so a sync merge that drops one fails here.
#
# Usage: moqtest_client_flags.sh <moqtest_client>

set -uo pipefail

CLIENT="${1:?usage: <moqtest_client>}"

fail=0

# expect <case> <pattern> <client args>...: the client's output for these
# arguments must contain the pattern.
expect() {
  local name="$1" pattern="$2"
  shift 2
  local out
  out="$("$CLIENT" "$@" 2>&1)"
  if grep -q -- "$pattern" <<<"$out"; then
    echo "PASS  $name"
  else
    echo "FAIL  $name: missing /$pattern/"
    tail -5 <<<"$out"
    fail=1
  fi
}

# The request type is checked only after connecting, so look at the help.
expect subscribe_tracks '"subscribe_tracks"' --helpon=MoQTestClientMain
# --groups is checked against the request before connecting. Reaching the
# datagram check means subscribe_tracks passed the request check.
expect groups 'only applies with --request=subscribe or subscribe_tracks' \
  --groups=1 --request=fetch
expect groups_subscribe_tracks 'does not support datagram' \
  --groups=1 --request=subscribe_tracks --forwarding_preference=3
# An unknown flag would stop the client before the --groups check.
expect ns_prefix 'only applies with' --ns_prefix=a --groups=1 --request=fetch

exit "$fail"
