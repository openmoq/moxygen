#!/usr/bin/env bash
# collect-artifacts-standalone.sh — Package a cmake install prefix into a release tarball.
#
# Strips debug symbols from libraries and creates a compressed tarball suitable
# for upload as a GitHub Release asset.
#
# Usage:
#   collect-artifacts-standalone.sh \
#     --install-prefix /path/to/install \
#     --output /path/to/moxygen-platform.tar.gz \
#     [--src-dir /path/to/moxygen-source]
#
# The --src-dir option is used to gather any headers not installed by cmake.

set -euo pipefail

INSTALL_PREFIX=""
OUTPUT=""
SRC_DIR=""

usage() {
  cat <<EOF
Usage: $(basename "$0") --install-prefix DIR --output FILE [--src-dir DIR]

Options:
  --install-prefix DIR   Path to the cmake install prefix
  --output FILE          Output tarball path (must end in .tar.gz)
  --src-dir DIR          Path to the moxygen source tree (for supplemental headers)
  -h, --help             Show this help
EOF
  exit "${1:-0}"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --install-prefix) INSTALL_PREFIX="$2"; shift 2 ;;
    --output)         OUTPUT="$2"; shift 2 ;;
    --src-dir)        SRC_DIR="$2"; shift 2 ;;
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

# ── Step 2: Supplement headers if needed ─────────────────────────────────────

if [[ -n "$SRC_DIR" && -d "$SRC_DIR" ]]; then
  # Check if moxygen headers were installed; if not, copy from source
  if [[ ! -d "$INSTALL_PREFIX/include/moxygen" ]]; then
    echo "==> Supplementing moxygen headers from source..."
    mkdir -p "$INSTALL_PREFIX/include/moxygen"
    find "$SRC_DIR/moxygen" -name '*.h' -exec cp --parents -t "$INSTALL_PREFIX/include/" {} + 2>/dev/null || \
    find "$SRC_DIR/moxygen" -name '*.h' | while read -r hdr; do
      REL="${hdr#$SRC_DIR/}"
      mkdir -p "$INSTALL_PREFIX/include/$(dirname "$REL")"
      cp "$hdr" "$INSTALL_PREFIX/include/$REL"
    done
    HEADER_COUNT=$(find "$INSTALL_PREFIX/include/moxygen" -name '*.h' | wc -l)
    echo "    Copied $HEADER_COUNT header files"
  fi
fi

# ── Step 3: Strip debug symbols ──────────────────────────────────────────────

echo "==> Stripping debug symbols..."

OS=$(uname -s)
STRIPPED=0

if [[ "$OS" == "Darwin" ]]; then
  while IFS= read -r -d '' lib; do
    strip -S "$lib" 2>/dev/null && STRIPPED=$((STRIPPED + 1)) || true
  done < <(find "$INSTALL_PREFIX" \( -name '*.a' -o -name '*.dylib' \) -type f -print0)
  echo "    macOS: stripped $STRIPPED libraries"

elif [[ "$OS" == "Linux" ]]; then
  while IFS= read -r -d '' lib; do
    if strip --strip-debug "$lib" 2>/dev/null; then
      STRIPPED=$((STRIPPED + 1))
    fi
  done < <(find "$INSTALL_PREFIX" \( -name '*.a' -o -name '*.so' -o -name '*.so.*' \) -type f -print0)
  echo "    Linux: stripped $STRIPPED libraries"

else
  echo "    Warning: unknown OS '$OS', skipping strip" >&2
fi

POST_STRIP_SIZE=$(du -sh "$INSTALL_PREFIX" | cut -f1)
echo "    Size after strip: $POST_STRIP_SIZE"

# ── Step 4: Create tarball ───────────────────────────────────────────────────

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
