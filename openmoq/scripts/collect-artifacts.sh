#!/usr/bin/env bash
# collect-artifacts.sh — Collect getdeps install dirs into a release tarball.
#
# Filters out build-tool-only dependencies, strips debug symbols (keeping
# .debug sidecar files on Linux), and produces a compressed tarball suitable
# for upload as a GitHub Release asset.
#
# Usage:
#   collect-artifacts.sh \
#     --scratch-path /path/to/moxygen-scratch \
#     --output /path/to/moxygen-platform.tar.gz \
#     --src-dir /path/to/moxygen-source
#
# Can be tested locally against any getdeps scratch directory.

set -euo pipefail

# ── Defaults ──────────────────────────────────────────────────────────────────

SCRATCH_PATH=""
OUTPUT=""
SRC_DIR=""

# Build tools that consumers never need. These are only used during the
# getdeps build itself and are not link-time or runtime dependencies.
EXCLUDE_DEPS="ninja cmake autoconf automake libtool gperf"

# ── Argument parsing ─────────────────────────────────────────────────────────

usage() {
  cat <<EOF
Usage: $(basename "$0") --scratch-path DIR --output FILE --src-dir DIR

Options:
  --scratch-path DIR   Path to the getdeps scratch directory
  --output FILE        Output tarball path (must end in .tar.gz)
  --src-dir DIR        Path to the moxygen source tree (for running getdeps.py)
  --exclude DEPS       Space-separated list of dep names to exclude
                       (default: "$EXCLUDE_DEPS")
  -h, --help           Show this help
EOF
  exit "${1:-0}"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --scratch-path) SCRATCH_PATH="$2"; shift 2 ;;
    --output)       OUTPUT="$2"; shift 2 ;;
    --src-dir)      SRC_DIR="$2"; shift 2 ;;
    --exclude)      EXCLUDE_DEPS="$2"; shift 2 ;;
    -h|--help)      usage 0 ;;
    *)              echo "Unknown option: $1" >&2; usage 1 ;;
  esac
done

if [[ -z "$SCRATCH_PATH" || -z "$OUTPUT" || -z "$SRC_DIR" ]]; then
  echo "Error: --scratch-path, --output, and --src-dir are all required." >&2
  usage 1
fi

if [[ ! -d "$SCRATCH_PATH" ]]; then
  echo "Error: scratch path does not exist: $SCRATCH_PATH" >&2
  exit 1
fi

GETDEPS="$SRC_DIR/build/fbcode_builder/getdeps.py"
if [[ ! -f "$GETDEPS" ]]; then
  echo "Error: getdeps.py not found at: $GETDEPS" >&2
  exit 1
fi

# ── Step 1: Get install directories ──────────────────────────────────────────

echo "==> Querying install directories from getdeps..."

INST_DIRS_FILE=$(mktemp)
trap 'rm -f "$INST_DIRS_FILE"' EXIT

python3 "$GETDEPS" \
  show-inst-dir \
  --scratch-path "$SCRATCH_PATH" \
  --extra-cmake-defines '{"CMAKE_FIND_LIBRARY_SUFFIXES": ".a", "BUILD_SHARED_LIBS": "OFF"}' \
  --recursive moxygen 2>/dev/null \
  | grep "^/" > "$INST_DIRS_FILE"

TOTAL_DIRS=$(wc -l < "$INST_DIRS_FILE")
echo "    Found $TOTAL_DIRS install directories"

# ── Step 2: Filter out build tools ───────────────────────────────────────────

echo "==> Filtering out build-tool dependencies: $EXCLUDE_DEPS"

FILTERED_FILE=$(mktemp)
trap 'rm -f "$INST_DIRS_FILE" "$FILTERED_FILE"' EXIT

# Build a grep pattern that matches install dir names for each build tool.
# getdeps names dirs as <name>-<hash>, e.g. /installed/cmake-oJhduB4y...
EXCLUDE_PATTERN=""
for dep in $EXCLUDE_DEPS; do
  if [[ -n "$EXCLUDE_PATTERN" ]]; then
    EXCLUDE_PATTERN="$EXCLUDE_PATTERN|"
  fi
  EXCLUDE_PATTERN="${EXCLUDE_PATTERN}/installed/${dep}(-|$)"
done

if [[ -n "$EXCLUDE_PATTERN" ]]; then
  grep -vE "$EXCLUDE_PATTERN" "$INST_DIRS_FILE" > "$FILTERED_FILE" || true
else
  cp "$INST_DIRS_FILE" "$FILTERED_FILE"
fi

KEPT_DIRS=$(wc -l < "$FILTERED_FILE")
EXCLUDED=$((TOTAL_DIRS - KEPT_DIRS))
echo "    Keeping $KEPT_DIRS directories (excluded $EXCLUDED build tools)"

# Show what was excluded
if [[ "$EXCLUDED" -gt 0 ]]; then
  echo "    Excluded:"
  comm -23 <(sort "$INST_DIRS_FILE") <(sort "$FILTERED_FILE") | while read -r dir; do
    echo "      - $(basename "$dir")"
  done
fi

# ── Step 3: Copy into staging area ───────────────────────────────────────────

echo "==> Staging artifacts..."

STAGE=$(mktemp -d)
trap 'rm -f "$INST_DIRS_FILE" "$FILTERED_FILE"; rm -rf "$STAGE"' EXIT

while IFS= read -r dir; do
  if [[ -d "$dir" ]]; then
    cp -a "$dir/." "$STAGE/"
  else
    echo "    Warning: directory not found, skipping: $dir" >&2
  fi
done < "$FILTERED_FILE"

# Save cmake_prefix_path for consumers
tr '\n' ';' < "$FILTERED_FILE" > "$STAGE/cmake_prefix_path.txt"

# Report pre-strip size
PRE_STRIP_SIZE=$(du -sh "$STAGE" | cut -f1)
echo "    Staged size (before strip): $PRE_STRIP_SIZE"

# ── Step 4: Strip debug symbols ─────────────────────────────────────────────

echo "==> Stripping debug symbols..."

OS=$(uname -s)
STRIPPED=0
DEBUG_CREATED=0

if [[ "$OS" == "Darwin" ]]; then
  # macOS: strip -S removes debug symbols, keeps symbol table.
  # No .debug sidecar generation (macOS uses dSYM bundles differently).
  while IFS= read -r -d '' lib; do
    strip -S "$lib" 2>/dev/null && STRIPPED=$((STRIPPED + 1)) || true
  done < <(find "$STAGE" \( -name '*.a' -o -name '*.dylib' \) -type f -print0)
  echo "    macOS: stripped $STRIPPED libraries (no .debug files generated)"

elif [[ "$OS" == "Linux" ]]; then
  # Linux: strip debug symbols from libraries.
  # .debug sidecar files are created in a separate staging area so they
  # can be packaged into an optional debug tarball without inflating the
  # main artifact.
  DEBUG_STAGE=$(mktemp -d)

  while IFS= read -r -d '' lib; do
    # Path relative to STAGE (e.g. lib/libfolly.a)
    REL="${lib#$STAGE/}"
    LIBNAME=$(basename "$lib")

    # Extract debug info if present (check for any debug/DWARF sections)
    if objdump -h "$lib" 2>/dev/null | grep -qE '\.(z?debug_|gnu\.debuglto_)'; then
      DEBUGDIR="$DEBUG_STAGE/$(dirname "$REL")/.debug"
      mkdir -p "$DEBUGDIR"
      if objcopy --only-keep-debug "$lib" "$DEBUGDIR/${LIBNAME}.debug" 2>/dev/null; then
        DEBUG_CREATED=$((DEBUG_CREATED + 1))
      fi
    fi

    # Always strip — catches debug info in all formats (DWARF4, DWARF5,
    # compressed .zdebug_*, split DWARF, LTO debug sections)
    if strip --strip-debug "$lib" 2>/dev/null; then
      STRIPPED=$((STRIPPED + 1))
    fi
  done < <(find "$STAGE" \( -name '*.a' -o -name '*.so' -o -name '*.so.*' \) -type f -print0)
  echo "    Linux: stripped $STRIPPED libraries, created $DEBUG_CREATED .debug files"

else
  echo "    Warning: unknown OS '$OS', skipping strip" >&2
fi

POST_STRIP_SIZE=$(du -sh "$STAGE" | cut -f1)
echo "    Staged size (after strip): $POST_STRIP_SIZE"

# ── Step 5: Create tarballs ──────────────────────────────────────────────────

# Ensure output directory exists
mkdir -p "$(dirname "$OUTPUT")"

# Main tarball (stripped libraries + headers + cmake configs)
echo "==> Creating tarball: $OUTPUT"
tar czf "$OUTPUT" -C "$STAGE" .

TARBALL_SIZE=$(du -sh "$OUTPUT" | cut -f1)
echo "    Tarball size: $TARBALL_SIZE"

# Debug symbols tarball (optional, separate download)
if [[ "$OS" == "Linux" && -n "${DEBUG_STAGE:-}" && "$DEBUG_CREATED" -gt 0 ]]; then
  DEBUG_OUTPUT="${OUTPUT%.tar.gz}-dbg.tar.gz"
  echo "==> Creating debug tarball: $DEBUG_OUTPUT"
  tar czf "$DEBUG_OUTPUT" -C "$DEBUG_STAGE" .
  DEBUG_SIZE=$(du -sh "$DEBUG_OUTPUT" | cut -f1)
  echo "    Debug tarball size: $DEBUG_SIZE"
  rm -rf "$DEBUG_STAGE"
fi

# Check against GitHub Release 2GB limit
TARBALL_BYTES=$(stat --format=%s "$OUTPUT" 2>/dev/null || stat -f%z "$OUTPUT" 2>/dev/null)
LIMIT=$((2 * 1024 * 1024 * 1024))  # 2 GiB
if [[ "$TARBALL_BYTES" -ge "$LIMIT" ]]; then
  echo "WARNING: Tarball exceeds GitHub Release 2 GiB limit ($TARBALL_SIZE)!" >&2
  echo "         Consider excluding more dependencies or verifying strip worked." >&2
  exit 1
fi

echo "==> Done. Summary:"
echo "    Install dirs: $TOTAL_DIRS total, $KEPT_DIRS kept, $EXCLUDED excluded"
echo "    Size: $PRE_STRIP_SIZE -> $POST_STRIP_SIZE (staged) -> $TARBALL_SIZE (compressed)"
