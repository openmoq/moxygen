# OpenMOQ Customizations

This directory contains OpenMOQ-specific files that are not part of the upstream
[facebookexperimental/moxygen](https://github.com/facebookexperimental/moxygen) tree.

## Directory Structure

- **`patches/`** — Ordered patch files applied during upstream sync. Named with
  numeric prefixes for application order (e.g., `001-relay-config-hooks.patch`).
  Applied sequentially via `git apply` or `git am` in the `openmoq-upstream-sync`
  workflow.

## Workflow Files

OpenMOQ-specific GitHub Actions workflows are prefixed with `openmoq-` to avoid
merge conflicts when syncing with the upstream repository:

- `.github/workflows/openmoq-upstream-sync.yml` — Detects upstream changes, creates
  candidate branch with patches, opens PR for validation.
- `.github/workflows/openmoq-publish-artifacts.yml` — Builds and publishes per-platform
  artifact bundles on merge to `main`.
