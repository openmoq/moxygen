# Contributing to openmoq/moxygen

This is openmoq's fork of
[facebookexperimental/moxygen](https://github.com/facebookexperimental/moxygen),
maintained with minimal divergence. Contributions welcome from anyone.

## Guiding principle

> Producing code in the era of AI is cheap. Reviewer attention is the
> scarce resource. We optimize our workflow for human review.

Because this is a fork: **prefer upstreaming over carrying local
changes.** A patch accepted upstream has zero sync cost forever; a
fork-local patch re-applies to every upstream sync.

## Pull request scope

**One PR = one cohesive thesis.** A reviewer should be able to read the
title and predict the diff.

- ✅ "fix: MoQForwarder::Subscriber::onPublishOk now updates forwardingSubscribers_"
- ❌ "various fixes and cleanups" (no thesis)
- ❌ "feature X + refactor Y" (split it)

## How to contribute

- **Outside contributors**: fork this repo, push a branch, open a PR
  against `main`.
- **Org members**: push a feature branch to this repo, open a PR
  against `main`.

CI runs on every PR with no secrets exposed. Publish, release, and
deploy only run on `push: main` after merge.

First-time fork PRs may show "Waiting for approval" on Actions — a
maintainer will unblock it; subsequent PRs from the same contributor
run automatically.

## Reviews

At least one approving review from a collaborator is required.
[CODEOWNERS](CODEOWNERS) auto-requests reviewers. Review is available
on GitHub or on
[Reviewable](https://reviewable.io/reviews/openmoq/moxygen); either is
fine.

## CI

Every PR must pass before merge:

- `check-format` — clang-format + license headers
- `linux` — build + test on Ubuntu 22.04
- `macos` — build + test on macOS
- `asan debug` — build + test with AddressSanitizer

See [.github/workflows/omoq-ci-pr.yml](.github/workflows/omoq-ci-pr.yml).
If a change needs a CI update (new dep, platform, flag), include the
workflow edit in the same PR.

## Branches

- `main` — the working branch; all openmoq customizations live here.
- `upstream` — mirror of `facebookexperimental/moxygen:main`, advanced
  automatically by the daily sync workflow. Do not push to it.
- `sync/<sha>` — sync PR branches opened by the sync bot when upstream
  advances. Collaborators may push conflict fixes to these.

## Upstream sync

A daily workflow
([.github/workflows/omoq-upstream-sync.yml](.github/workflows/omoq-upstream-sync.yml))
mirrors `facebookexperimental/moxygen:main` to `upstream` and opens a
`sync/<sha>` PR against `main` when upstream advances. The PR
auto-merges on green CI; conflicts are resolved by pushing fixes to the
`sync/<sha>` branch.

For files that conflict repeatedly (`cmake/moxygen-config.cmake.in`,
`moxygen/CMakeLists.txt`), prefer upstreaming a fix to Meta rather than
carrying a local patch.

## Merge

PRs are squash-merged; the PR title becomes the commit message on
`main`, so write titles that summarize the change well. Branch commit
organization (rebase, amend, multiple commits) is up to the author —
it has no effect on the merged result.

Maintainers with merge rights: @afrind, @gmarzot, @suhasHere. The
`omoq-sync-bot` GitHub App merges upstream-sync PRs automatically.

## Security & License

Report security issues via [SECURITY.md](SECURITY.md). By contributing
you agree your changes are licensed under [LICENSE](LICENSE).
