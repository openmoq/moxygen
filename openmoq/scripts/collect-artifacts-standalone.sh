#!/usr/bin/env bash
# collect-artifacts-standalone.sh — Package a cmake install prefix into a release tarball.
#
# Strips debug symbols from libraries and creates a compressed tarball suitable
# for upload as a GitHub Release asset. Optionally extracts debug symbols into
# separate .debug sidecar files (split debug) and packages them as a second tarball.
#
# Usage:
#   collect-artifacts-standalone.sh \
#     --install-prefix /path/to/install \
#     --output /path/to/moxygen-platform.tar.gz \
#     [--debug-output /path/to/moxygen-platform-dbg.tar.gz]
#
# The --debug-output option extracts split debug symbols before stripping.

set -euo pipefail

INSTALL_PREFIX=""
OUTPUT=""
DEBUG_OUTPUT=""

usage() {
  cat <<EOF
Usage: $(basename "$0") --install-prefix DIR --output FILE [--debug-output FILE]

Options:
  --install-prefix DIR   Path to the cmake install prefix
  --output FILE          Output tarball path (must end in .tar.gz)
  --debug-output FILE    Create a separate tarball of unstripped libs before stripping
  -h, --help             Show this help
EOF
  exit "${1:-0}"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --install-prefix) INSTALL_PREFIX="$2"; shift 2 ;;
    --output)         OUTPUT="$2"; shift 2 ;;
    --debug-output)   DEBUG_OUTPUT="$2"; shift 2 ;;
    -h|--help)        usage 0 ;;
    *)                echo "Unknown option: $1" >&2; usage 1 ;;
  esac
done

if [[ -z "$INSTALL_PREFIX" || -z "$OUTPUT" ]]; then
  echo "Error: --install-prefix and --output are required." >&2
  usage 1
fi

if [[ ! -d "$INSTALL_PREFIX" ]]; then
  echo "Error: install prefix does not exist: $INSTALL_PREFIX" >&2
  exit 1
fi

# ── Step 0: Drop binaries that are not part of the relay artifact ────────────
# Built under BUILD_SAMPLES beside binaries we do ship, so excluded here.
rm -f "$INSTALL_PREFIX"/bin/moq_media_server* "$INSTALL_PREFIX"/bin/moq_mp4_receiver*

# ── Step 1: Report contents ──────────────────────────────────────────────────

echo "==> Install prefix: $INSTALL_PREFIX"
echo "    Directories:"
for d in lib include bin share lib64; do
  if [[ -d "$INSTALL_PREFIX/$d" ]]; then
    COUNT=$(find "$INSTALL_PREFIX/$d" -type f | wc -l)
    echo "      $d/ ($COUNT files)"
  fi
done

PRE_STRIP_SIZE=$(du -sh "$INSTALL_PREFIX" | cut -f1)
echo "    Total size (before strip): $PRE_STRIP_SIZE"

# ── Step 2: Extract split debug symbols and strip ────────────────────────────

echo "==> Stripping debug symbols..."

OS=$(uname -s)
STRIPPED=0
DEBUG_DIR=""

# Set up debug output directory if requested
if [[ -n "$DEBUG_OUTPUT" ]]; then
  DEBUG_DIR=$(mktemp -d)
fi

# Sample binaries ship stripped but without sidecars: each statically links the
# whole folly/proxygen/mvfst stack, so a sidecar apiece adds ~130 MB of duplicate
# DWARF and pushes the -dbg tarball past GitHub's 2 GB asset limit.
NO_SIDECAR_BINS='moqchatclient moqdateserver moqflvreceiverclient moqflvstreamerclient moqtextclient'

wants_sidecar() {
  local name
  name=$(basename "$1")
  case " $NO_SIDECAR_BINS " in
    *" $name "*) return 1 ;;
  esac
}

if [[ "$OS" == "Darwin" ]]; then
  while IFS= read -r -d '' lib; do
    # No .a sidecars — see the Linux loop.
    if [[ -n "$DEBUG_DIR" && "$lib" != *.a ]]; then
      REL="${lib#$INSTALL_PREFIX/}"
      mkdir -p "$DEBUG_DIR/$(dirname "$REL")"
      # macOS: copy unstripped lib as debug sidecar (no objcopy equivalent)
      cp "$lib" "$DEBUG_DIR/${REL}.debug" 2>/dev/null || true
    fi
    strip -S "$lib" 2>/dev/null && STRIPPED=$((STRIPPED + 1)) || true
  done < <(find "$INSTALL_PREFIX" \( -name '*.a' -o -name '*.dylib' \) -type f -print0)
  # Executables under bin/ — same treatment.
  if [[ -d "$INSTALL_PREFIX/bin" ]]; then
    while IFS= read -r -d '' exe; do
      if [[ -n "$DEBUG_DIR" ]] && wants_sidecar "$exe"; then
        REL="${exe#$INSTALL_PREFIX/}"
        mkdir -p "$DEBUG_DIR/$(dirname "$REL")"
        cp "$exe" "$DEBUG_DIR/${REL}.debug" 2>/dev/null || true
      fi
      strip -S "$exe" 2>/dev/null && STRIPPED=$((STRIPPED + 1)) || true
    done < <(find "$INSTALL_PREFIX/bin" -type f -perm -u+x -print0)
  fi
  echo "    macOS: stripped $STRIPPED files"

elif [[ "$OS" == "Linux" ]]; then
  while IFS= read -r -d '' lib; do
    REL="${lib#$INSTALL_PREFIX/}"
    # No .a sidecars: every executable that links an archive already carries
    # its DWARF, and nothing debugs a .a.
    if [[ -n "$DEBUG_DIR" && "$lib" != *.a ]]; then
      mkdir -p "$DEBUG_DIR/$(dirname "$REL")"
      # Compressed DWARF (zlib, gdb/elfutils-readable) halves sidecar size.
      objcopy --only-keep-debug --compress-debug-sections "$lib" "$DEBUG_DIR/${REL}.debug" 2>/dev/null || true
    fi
    if strip --strip-debug "$lib" 2>/dev/null; then
      # Add debuglink so gdb can find the sidecar automatically
      if [[ -n "$DEBUG_DIR" && -f "$DEBUG_DIR/${REL}.debug" ]]; then
        objcopy --add-gnu-debuglink="$DEBUG_DIR/${REL}.debug" "$lib" 2>/dev/null || true
      fi
      STRIPPED=$((STRIPPED + 1))
    fi
  done < <(find "$INSTALL_PREFIX" \( -name '*.a' -o -name '*.so' -o -name '*.so.*' \) -type f -print0)
  # Executables under bin/ — same treatment as libs. Walked explicitly: they
  # typically have no extension, so the lib find above misses them.
  if [[ -d "$INSTALL_PREFIX/bin" ]]; then
    while IFS= read -r -d '' exe; do
      REL="${exe#$INSTALL_PREFIX/}"
      if [[ -n "$DEBUG_DIR" ]] && wants_sidecar "$exe"; then
        mkdir -p "$DEBUG_DIR/$(dirname "$REL")"
        objcopy --only-keep-debug --compress-debug-sections "$exe" "$DEBUG_DIR/${REL}.debug" 2>/dev/null || true
      fi
      if strip --strip-debug "$exe" 2>/dev/null; then
        if [[ -n "$DEBUG_DIR" && -f "$DEBUG_DIR/${REL}.debug" ]]; then
          objcopy --add-gnu-debuglink="$DEBUG_DIR/${REL}.debug" "$exe" 2>/dev/null || true
        fi
        STRIPPED=$((STRIPPED + 1))
      fi
    done < <(find "$INSTALL_PREFIX/bin" -type f -perm -u+x -print0)
  fi
  echo "    Linux: stripped $STRIPPED files"

else
  echo "    Warning: unknown OS '$OS', skipping strip" >&2
fi

POST_STRIP_SIZE=$(du -sh "$INSTALL_PREFIX" | cut -f1)
echo "    Size after strip: $POST_STRIP_SIZE"

# ── Step 3: Create debug tarball ─────────────────────────────────────────────

if [[ -n "$DEBUG_OUTPUT" && -n "$DEBUG_DIR" ]]; then
  DBG_FILE_COUNT=$(find "$DEBUG_DIR" -name '*.debug' -type f | wc -l)
  if [[ "$DBG_FILE_COUNT" -gt 0 ]]; then
    mkdir -p "$(dirname "$DEBUG_OUTPUT")"
    echo "==> Creating debug tarball: $DEBUG_OUTPUT ($DBG_FILE_COUNT debug files)"
    tar czf "$DEBUG_OUTPUT" -C "$DEBUG_DIR" .
    DBG_SIZE=$(du -sh "$DEBUG_OUTPUT" | cut -f1)
    echo "    Debug tarball size: $DBG_SIZE"
    # Same 2 GiB release-asset limit as the main tarball.
    DBG_BYTES=$(stat --format=%s "$DEBUG_OUTPUT" 2>/dev/null || stat -f%z "$DEBUG_OUTPUT" 2>/dev/null)
    if [[ "$DBG_BYTES" -ge $((2 * 1024 * 1024 * 1024)) ]]; then
      echo "ERROR: Debug tarball exceeds GitHub Release 2 GiB limit ($DBG_SIZE)!" >&2
      exit 1
    fi
  else
    echo "==> No debug files extracted, skipping debug tarball"
  fi
  rm -rf "$DEBUG_DIR"
fi

# ── Step 4: Create release tarball (stripped) ─────────────────────────────────

mkdir -p "$(dirname "$OUTPUT")"

echo "==> Creating tarball: $OUTPUT"
tar czf "$OUTPUT" -C "$INSTALL_PREFIX" .

TARBALL_SIZE=$(du -sh "$OUTPUT" | cut -f1)
echo "    Tarball size: $TARBALL_SIZE"

# Check against GitHub Release 2GB limit (safety net)
TARBALL_BYTES=$(stat --format=%s "$OUTPUT" 2>/dev/null || stat -f%z "$OUTPUT" 2>/dev/null)
LIMIT=$((2 * 1024 * 1024 * 1024))
if [[ "$TARBALL_BYTES" -ge "$LIMIT" ]]; then
  echo "ERROR: Tarball exceeds GitHub Release 2 GiB limit ($TARBALL_SIZE)!" >&2
  exit 1
fi

echo "==> Done: $PRE_STRIP_SIZE -> $POST_STRIP_SIZE (stripped) -> $TARBALL_SIZE (compressed)"
