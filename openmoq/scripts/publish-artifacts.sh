#!/usr/bin/env bash
# publish-artifacts.sh — Publish build artifacts as a GitHub pre-release.
#
# Creates (or replaces) a pre-release tagged at the given commit SHA. Two modes:
#
#   --artifacts-dir DIR   pinned snapshot (e.g. snapshot-<sha12>): uploads all
#                         .tar.gz files; retained so pin-following consumers
#                         (moqx MOXYGEN_REV) can fetch this exact rev
#   --pointer-to TAG      rolling alias (snapshot-latest): asset-less release
#                         whose notes link to the pinned release — assets are
#                         uploaded once, to the pinned release only
#
# --prune-days N deletes pinned snapshot-<sha12> pre-releases (and their tags)
# older than N days after a successful publish.
#
# Requires: gh CLI authenticated with a token that has contents:write.

set -euo pipefail

# ── Defaults ──────────────────────────────────────────────────────────────────

ARTIFACTS_DIR=""
SHA=""
TAG="snapshot-latest"
BRANCH="main"
REPO=""  # defaults to current repo if empty
POINTER_TO=""
PRUNE_DAYS=0
DRY_RUN=false

# ── Argument parsing ─────────────────────────────────────────────────────────

usage() {
  cat <<EOF
Usage: $(basename "$0") --artifacts-dir DIR --sha SHA [OPTIONS]

Options:
  --artifacts-dir DIR   Directory containing .tar.gz artifact files
  --pointer-to TAG      Publish an asset-less pointer release linking to TAG
                        (mutually exclusive with --artifacts-dir)
  --sha SHA             Full commit SHA for the release
  --tag TAG             Pre-release tag name (default: snapshot-latest)
  --branch BRANCH       Source branch name for release notes (default: main)
  --repo OWNER/REPO     GitHub repository (default: current repo from gh)
  --prune-days N        After publishing, delete pinned snapshot-<sha12>
                        pre-releases older than N days (default: 0 = off)
  --dry-run             Show what would be done without creating the release
  -h, --help            Show this help
EOF
  exit "${1:-0}"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --artifacts-dir) ARTIFACTS_DIR="$2"; shift 2 ;;
    --pointer-to)    POINTER_TO="$2"; shift 2 ;;
    --sha)           SHA="$2"; shift 2 ;;
    --tag)           TAG="$2"; shift 2 ;;
    --branch)        BRANCH="$2"; shift 2 ;;
    --repo)          REPO="$2"; shift 2 ;;
    --prune-days)    PRUNE_DAYS="$2"; shift 2 ;;
    --dry-run)       DRY_RUN=true; shift ;;
    -h|--help)       usage 0 ;;
    *)               echo "Unknown option: $1" >&2; usage 1 ;;
  esac
done

if [[ -z "$SHA" ]]; then
  echo "Error: --sha is required." >&2
  usage 1
fi

if [[ -n "$ARTIFACTS_DIR" && -n "$POINTER_TO" ]] || [[ -z "$ARTIFACTS_DIR" && -z "$POINTER_TO" ]]; then
  echo "Error: exactly one of --artifacts-dir or --pointer-to is required." >&2
  usage 1
fi

if [[ -n "$ARTIFACTS_DIR" && ! -d "$ARTIFACTS_DIR" ]]; then
  echo "Error: artifacts directory does not exist: $ARTIFACTS_DIR" >&2
  exit 1
fi

REPO_FLAG=""
if [[ -n "$REPO" ]]; then
  REPO_FLAG="--repo $REPO"
fi

SHORT_SHA="${SHA:0:7}"

# The pointer link and the prune API need the slug even when --repo is unset.
REPO_SLUG="${REPO:-$(gh repo view --json nameWithOwner --jq .nameWithOwner)}"

# ── Step 1: Collect artifact files (pinned mode only) ────────────────────────

RELEASE_DIR=$(mktemp -d)
trap 'rm -rf "$RELEASE_DIR"' EXIT
ASSET_COUNT=0

if [[ -z "$POINTER_TO" ]]; then
  echo "==> Collecting artifacts from: $ARTIFACTS_DIR"

  # download-artifact@v4 creates a subdirectory per artifact name.
  # Flatten: find all .tar.gz files regardless of nesting depth.
  while IFS= read -r -d '' tarball; do
    cp "$tarball" "$RELEASE_DIR/"
    ASSET_COUNT=$((ASSET_COUNT + 1))
    SIZE=$(du -sh "$tarball" | cut -f1)
    echo "    $(basename "$tarball"): $SIZE"
  done < <(find "$ARTIFACTS_DIR" -name '*.tar.gz' -type f -print0)

  if [[ "$ASSET_COUNT" -eq 0 ]]; then
    echo "Error: no .tar.gz files found in $ARTIFACTS_DIR" >&2
    exit 1
  fi

  echo "    Found $ASSET_COUNT artifact(s)"
fi

# ── Step 2: Upload with retry ────────────────────────────────────────────────

upload_with_retry() {
  local asset="$1"
  local name
  name=$(basename "$asset")
  local max=3 delay=10 attempt=1
  while [[ $attempt -le $max ]]; do
    # shellcheck disable=SC2086
    if gh release upload "$TAG" "$asset" --clobber $REPO_FLAG; then
      return 0
    fi
    if [[ $attempt -lt $max ]]; then
      echo "    Upload failed (attempt $attempt/$max), retrying in ${delay}s..."
      sleep "$delay"
      delay=$((delay * 2))
    fi
    attempt=$((attempt + 1))
  done
  echo "    ERROR: $name upload failed after $max attempts" >&2
  return 1
}

# ── Step 3: Create/replace the release ───────────────────────────────────────

# Rolling aliases move every push; pinned snapshots describe one rev forever.
ROLLING=false
case "$TAG" in
  snapshot-latest|snapshot-*-latest) ROLLING=true ;;
esac

# Asset-bearing releases explain their tarballs; the pointer has none.
TARBALL_FOOTER="

Each platform has two tarballs:
- \`moxygen-<platform>.tar.gz\` — stripped release build
- \`moxygen-<platform>-dbg.tar.gz\` — split debug symbols (.debug sidecar files)"

if [[ -n "$POINTER_TO" ]]; then
  NOTES_BODY="Rolling pointer to the latest build from \`${BRANCH}\`.

**Commit:** \`${SHA}\`
**Artifacts:** [\`${POINTER_TO}\`](https://github.com/${REPO_SLUG}/releases/tag/${POINTER_TO})

Updated on every push to \`${BRANCH}\`. Artifacts are uploaded once, to the
pinned per-rev release this points at."
elif [[ "$ROLLING" == true ]]; then
  NOTES_BODY="Rolling snapshot of the latest build from \`${BRANCH}\`.

**Commit:** \`${SHA}\`
**Built:** $(date -u +%Y-%m-%dT%H:%M:%SZ)

This pre-release is automatically replaced on every push to \`${BRANCH}\`.${TARBALL_FOOTER}"
else
  NOTES_BODY="Pinned snapshot of the build at \`${SHORT_SHA}\` (\`${BRANCH}\`).

**Commit:** \`${SHA}\`
**Built:** $(date -u +%Y-%m-%dT%H:%M:%SZ)

Retained so pin-following consumers (moqx \`MOXYGEN_REV\`) can fetch prebuilts
for this exact revision.${TARBALL_FOOTER}"
fi

echo "==> Publishing snapshot: $TAG (commit $SHORT_SHA)"

if [[ "$DRY_RUN" == true ]]; then
  echo "    [dry-run] Would delete existing release $TAG"
  if [[ -n "$POINTER_TO" ]]; then
    echo "    [dry-run] Would create asset-less pointer pre-release $TAG -> $POINTER_TO"
  else
    echo "    [dry-run] Would create pre-release $TAG with $ASSET_COUNT assets"
  fi
else
  # Delete any existing release/tag with this name: replacement for rolling
  # aliases, idempotent re-publish (workflow rerun) for pinned snapshots.
  # shellcheck disable=SC2086
  gh release delete "$TAG" --yes $REPO_FLAG 2>/dev/null || true
  git tag -d "$TAG" 2>/dev/null || true
  git push origin ":refs/tags/$TAG" 2>/dev/null || true

  if [[ "$ROLLING" == true ]]; then
    TITLE="Latest build — ${BRANCH} (${SHORT_SHA})"
  else
    TITLE="Build — ${BRANCH} (${SHORT_SHA})"
  fi

  # Create as pre-release so it doesn't show as "Latest release"
  # --target must stay an explicit sha: downstream fetchers verify
  # target_commitish; gh defaults to a branch name for existing tags.
  # shellcheck disable=SC2086
  gh release create "$TAG" \
    --target "$SHA" \
    --title "$TITLE" \
    --prerelease \
    --notes "$NOTES_BODY" \
    $REPO_FLAG

  # Upload each asset individually with retry (pinned mode; the pointer has none)
  if [[ -z "$POINTER_TO" ]]; then
    for asset in "$RELEASE_DIR"/*.tar.gz; do
      echo "    Uploading $(basename "$asset")..."
      upload_with_retry "$asset"
    done
  fi

  # Ensure the release is not stuck as draft
  # (gh release create without files may leave it in draft state)
  RELEASE_ID=$(gh api repos/{owner}/{repo}/releases \
    --jq ".[] | select(.tag_name == \"$TAG\") | .id")
  if [[ -n "$RELEASE_ID" ]]; then
    gh api "repos/{owner}/{repo}/releases/$RELEASE_ID" \
      -X PATCH -f draft=false >/dev/null
    echo "    Release published (draft=false)"
  fi

  echo "    Snapshot published: $TAG"
fi

# ── Step 4: Prune aged pinned snapshots ──────────────────────────────────────

if [[ "$PRUNE_DAYS" -gt 0 ]]; then
  echo "==> Pruning pinned snapshots older than ${PRUNE_DAYS} days"
  CUTOFF=$(date -u -d "-${PRUNE_DAYS} days" +%s)
  gh api "repos/${REPO_SLUG}/releases" --paginate \
    --jq '.[] | select(.prerelease) | [.tag_name, .created_at] | @tsv' |
  while IFS=$'\t' read -r tag created; do
    # Pinned snapshots only — never rolling aliases or v* releases.
    [[ "$tag" =~ ^snapshot-[0-9a-f]{12}$ ]] || continue
    [[ "$tag" == "$TAG" ]] && continue
    created_s=$(date -u -d "$created" +%s)
    if (( created_s < CUTOFF )); then
      if [[ "$DRY_RUN" == true ]]; then
        echo "    [dry-run] Would delete $tag (created $created)"
      else
        echo "    Deleting $tag (created $created)"
        # shellcheck disable=SC2086
        gh release delete "$tag" --yes --cleanup-tag $REPO_FLAG \
          || echo "    WARNING: failed to delete $tag" >&2
      fi
    fi
  done
fi

echo "==> Done."
