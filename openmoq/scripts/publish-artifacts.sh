#!/usr/bin/env bash
# publish-artifacts.sh — Publish build artifacts as a GitHub pre-release.
#
# Creates (or replaces) a pre-release tagged at the given commit SHA. Modes:
#
#   --artifacts-dir DIR   pinned snapshot (e.g. snapshot-<sha12>): uploads all
#                         .tar.gz files; retained so pin-following consumers
#                         (moqx MOXYGEN_REV) can fetch this exact rev
#   --pointer-to TAG      rolling alias (snapshot-latest): asset-less release
#                         whose notes link to the pinned release — assets are
#                         uploaded once, to the pinned release only
#
# Or, to let each build job upload its own assets instead of funnelling them
# through one job, the same pinned snapshot in three steps:
#
#   --create-draft        create the pinned snapshot as a draft (no assets).
#                         Drafts are invisible to consumers and to by-tag
#                         lookups, so the release is never public half-filled
#   --upload-only         upload --artifacts-dir into an existing draft
#   --finalize            flip the draft public (then --prune-days applies)
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
CREATE_DRAFT=false
UPLOAD_ONLY=false
FINALIZE=false

# Assets upload concurrently, each attempt bounded: a single hung PUT to
# uploads.github.com would otherwise stall the release indefinitely.
UPLOAD_JOBS="${UPLOAD_JOBS:-4}"
UPLOAD_TIMEOUT="${UPLOAD_TIMEOUT:-1200}"

# ── Argument parsing ─────────────────────────────────────────────────────────

usage() {
  cat <<EOF
Usage: $(basename "$0") --artifacts-dir DIR --sha SHA [OPTIONS]

Options:
  --artifacts-dir DIR   Directory containing .tar.gz artifact files
  --pointer-to TAG      Publish an asset-less pointer release linking to TAG
                        (mutually exclusive with --artifacts-dir)
  --create-draft        Create the release as an empty draft
  --upload-only         Upload --artifacts-dir into an existing draft
  --finalize            Flip an existing draft public
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
    --create-draft)  CREATE_DRAFT=true; shift ;;
    --upload-only)   UPLOAD_ONLY=true; shift ;;
    --finalize)      FINALIZE=true; shift ;;
    --dry-run)       DRY_RUN=true; shift ;;
    -h|--help)       usage 0 ;;
    *)               echo "Unknown option: $1" >&2; usage 1 ;;
  esac
done

if [[ -z "$SHA" ]]; then
  echo "Error: --sha is required." >&2
  usage 1
fi

MODES=0
[[ -n "$POINTER_TO" ]] && MODES=$((MODES + 1))
[[ "$CREATE_DRAFT" == true ]] && MODES=$((MODES + 1))
[[ "$UPLOAD_ONLY" == true ]] && MODES=$((MODES + 1))
[[ "$FINALIZE" == true ]] && MODES=$((MODES + 1))
# A bare --artifacts-dir is the all-in-one mode; with --upload-only it is the
# source directory instead, so it only counts as a mode on its own.
[[ -n "$ARTIFACTS_DIR" && "$UPLOAD_ONLY" == false ]] && MODES=$((MODES + 1))

if [[ "$MODES" -ne 1 ]]; then
  echo "Error: exactly one of --artifacts-dir, --pointer-to, --create-draft," >&2
  echo "       --upload-only or --finalize is required." >&2
  usage 1
fi

if [[ "$UPLOAD_ONLY" == true && -z "$ARTIFACTS_DIR" ]]; then
  echo "Error: --upload-only requires --artifacts-dir." >&2
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

if [[ -n "$ARTIFACTS_DIR" ]]; then
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

# timeout exits 124, which this loop retries like any other failure.
upload_with_retry() {
  local asset="$1"
  local name
  name=$(basename "$asset")
  local max=3 delay=10 attempt=1
  while [[ $attempt -le $max ]]; do
    # shellcheck disable=SC2086
    if timeout --kill-after=30s "$UPLOAD_TIMEOUT" \
         gh release upload "$TAG" "$asset" --clobber $REPO_FLAG; then
      echo "    Uploaded $name"
      return 0
    fi
    if [[ $attempt -lt $max ]]; then
      echo "    $name: attempt $attempt/$max failed, retrying in ${delay}s..."
      sleep "$delay"
      delay=$((delay * 2))
    fi
    attempt=$((attempt + 1))
  done
  echo "    ERROR: $name upload failed after $max attempts" >&2
  return 1
}
export -f upload_with_retry

# Upload everything staged in RELEASE_DIR concurrently.
# Distinct asset names, so --clobber cannot race between jobs.
upload_assets() {
  export TAG REPO_FLAG UPLOAD_TIMEOUT
  echo "    Uploading $ASSET_COUNT asset(s), $UPLOAD_JOBS at a time..."
  if ! find "$RELEASE_DIR" -name '*.tar.gz' -type f -print0 |
         xargs -0 -P "$UPLOAD_JOBS" -n1 -I{} \
           bash -c 'upload_with_retry "$1"' _ {}; then
    echo "Error: one or more asset uploads failed." >&2
    exit 1
  fi
}

prune_snapshots() {
  [[ "$PRUNE_DAYS" -gt 0 ]] || return 0
  echo "==> Pruning pinned snapshots older than ${PRUNE_DAYS} days"
  local cutoff tag created created_s
  cutoff=$(date -u -d "-${PRUNE_DAYS} days" +%s)
  gh api "repos/${REPO_SLUG}/releases" --paginate \
    --jq '.[] | select(.prerelease) | [.tag_name, .created_at] | @tsv' |
  while IFS=$'\t' read -r tag created; do
    # Pinned snapshots only — never rolling aliases or v* releases.
    [[ "$tag" =~ ^snapshot-[0-9a-f]{12}$ ]] || continue
    [[ "$tag" == "$TAG" ]] && continue
    created_s=$(date -u -d "$created" +%s)
    if (( created_s < cutoff )); then
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
}

# ── Act on an existing release ───────────────────────────────────────────────

if [[ "$UPLOAD_ONLY" == true ]]; then
  echo "==> Uploading to $TAG"
  if [[ "$DRY_RUN" == true ]]; then
    echo "    [dry-run] Would upload $ASSET_COUNT asset(s) to $TAG"
  else
    upload_assets
  fi
  echo "==> Done."
  exit 0
fi

if [[ "$FINALIZE" == true ]]; then
  echo "==> Finalizing $TAG"
  if [[ "$DRY_RUN" == true ]]; then
    echo "    [dry-run] Would flip $TAG out of draft"
  else
    # shellcheck disable=SC2086
    gh release edit "$TAG" --draft=false $REPO_FLAG
    echo "    Release published (draft=false)"
  fi
  prune_snapshots
  echo "==> Done."
  exit 0
fi

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

  # A failed earlier run leaves an invisible draft. Drafts hold no tag ref, so
  # the delete above misses them and a second create would not collide — then
  # uploads-by-tag would be ambiguous. Paginate: on a busy repo an old draft
  # falls past the first page within hours.
  gh api --paginate "repos/${REPO_SLUG}/releases" \
    --jq ".[] | select(.tag_name == \"$TAG\" and .draft) | .id" |
  while read -r id; do
    echo "    Removing stale draft release $id for $TAG"
    gh api -X DELETE "repos/${REPO_SLUG}/releases/$id" || true
  done

  if [[ "$ROLLING" == true ]]; then
    TITLE="Latest build — ${BRANCH} (${SHORT_SHA})"
  else
    TITLE="Build — ${BRANCH} (${SHORT_SHA})"
  fi

  # --draft keeps the release invisible to consumers and to by-tag lookups
  # while its assets arrive; --finalize flips it once they all have.
  DRAFT_FLAG=()
  [[ "$CREATE_DRAFT" == true ]] && DRAFT_FLAG=(--draft)

  # Create as pre-release so it doesn't show as "Latest release"
  # --target must stay an explicit sha: downstream fetchers verify
  # target_commitish; gh defaults to a branch name for existing tags.
  # shellcheck disable=SC2086
  gh release create "$TAG" \
    --target "$SHA" \
    --title "$TITLE" \
    --prerelease \
    "${DRAFT_FLAG[@]}" \
    --notes "$NOTES_BODY" \
    $REPO_FLAG

  # The pointer release and a fresh draft both start with no assets.
  if [[ -z "$POINTER_TO" && "$CREATE_DRAFT" == false ]]; then
    upload_assets
  fi

  if [[ "$CREATE_DRAFT" == true ]]; then
    echo "    Draft created: $TAG"
  else
    # gh release create without files can leave the release in draft state.
    RELEASE_ID=$(gh api repos/{owner}/{repo}/releases \
      --jq ".[] | select(.tag_name == \"$TAG\") | .id")
    if [[ -n "$RELEASE_ID" ]]; then
      gh api "repos/{owner}/{repo}/releases/$RELEASE_ID" \
        -X PATCH -f draft=false >/dev/null
      echo "    Release published (draft=false)"
    fi
    echo "    Snapshot published: $TAG"
  fi
fi

# ── Step 4: Prune aged pinned snapshots ──────────────────────────────────────

prune_snapshots

echo "==> Done."
