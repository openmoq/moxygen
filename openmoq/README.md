# OpenMOQ Customizations

This directory contains OpenMOQ-specific files that are not part of the upstream
[facebookexperimental/moxygen](https://github.com/facebookexperimental/moxygen) tree.

## Branch Architecture

- **`upstream`** — Pure upstream mirror. Unmodified copy of the latest green upstream
  commit. No workflows run.
- **`main`** (default) — Working branch with all OpenMOQ customizations.
  Developer PRs go here. Artifacts published from here.

See [GITHUB_WORKFLOW.md](https://github.com/openmoq/moqx/blob/main/design/GITHUB_WORKFLOW.md)
for the full design.

## Directory Structure

- **`patches/`** — Reserved for post-merge patches if needed (e.g., changes to
  generated files that git merge cannot handle).
- **`scripts/`** — Build and CI scripts extracted from workflows for local testing.

## Workflows

OpenMOQ workflows live on `main` and are prefixed `omoq-` to avoid collisions
with upstream workflow files:

- `omoq-upstream-sync.yml` (`upstream sync`) — Daily: opens a `sync/<sha>`
  PR to `main` for the newest upstream commit with green CI, one at a time.
- `omoq-picoquic-sync.yml` (`picoquic sync`) — Daily: opens a
  `sync-picoquic/<sha>` PR when the openmoq/picoquic pin in
  `build/deps/github_hashes/openmoq/picoquic-rev.txt` falls behind.
- `omoq-auto-merge-sync.yml` (`sync auto-merge`) — Merges a sync PR when
  `ci pr` passes on its branch; notifies Slack and email when it fails.
- `omoq-ci-pr.yml` (`ci pr`) — Standalone build and tests for PRs to `main`:
  linux, macos and asan debug.
- `omoq-gemini-review.yml` (`🔎 Gemini Review`) — Gemini review, or answers
  to `/gemini` questions, when the `gemini-review` label is added to a PR.
- `omoq-ci-main.yml` (`ci main`) — On push to `main` or `release/**`: build
  and test, publish per-platform tarballs as a `snapshot-<sha12>`
  pre-release, and move the rolling `snapshot-latest` alias.
- `omoq-version-release.yml` (`version release`) — Manual: tags a versioned
  release with portable Linux tarballs plus the snapshot's platform tarballs.
- `omoq-dev-build.yml` (`dev build (manual)`) — Manual: snapshot-equivalent
  tarballs from any branch, as workflow artifacts.
- `omoq-pr-cache-cleanup.yml` (`pr cache cleanup`) — Deletes a PR's caches
  when it closes.

## Scripts

### `openmoq/scripts/publish-artifacts.sh`

Publishes build artifacts as a rolling `snapshot-latest` GitHub pre-release.

```bash
openmoq/scripts/publish-artifacts.sh \
  --artifacts-dir ./my-artifacts \
  --sha "$(git rev-parse HEAD)" \
  --dry-run
```

`ci main` instead splits this across jobs, so each build uploads its own
tarballs rather than funnelling them all through one job:

```bash
# create-release: empty draft, invisible until finalized
--create-draft --sha "$SHA" --tag snapshot-<sha12>

# each publish job: its own two tarballs
--upload-only --artifacts-dir _assets --sha "$SHA" --tag snapshot-<sha12>

# release: make it public, then prune aged snapshots
--finalize --sha "$SHA" --tag snapshot-<sha12> --prune-days 30
```

### `openmoq/scripts/collect-artifacts-standalone.sh`

Strips binaries, splits debug symbols, and creates release tarballs from a
cmake install prefix.

## Building Moxygen

### Standalone build (recommended)

```bash
# Install system deps
./standalone/install-system-deps.sh

# Build
cmake -B _build -S standalone -G Ninja
cmake --build _build -j$(getconf _NPROCESSORS_ONLN)

# Test
ctest --test-dir _build --output-on-failure

# Install
cmake --install _build --prefix /tmp/moxygen-install
```
