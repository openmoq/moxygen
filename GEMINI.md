# Gemini Code Review Guidelines for `moxygen`

`openmoq/moxygen` is a community-maintained fork of `facebookexperimental/moxygen`. moxygen is a C++20 Media over QUIC Transport (MOQT) library on `folly`, running over `mvfst` (via `proxygen`) and `picoquic`: the session layer, relay, cache and forwarder.

## Review Format & Output Rules

- **Skip high-level PR summary poems and walkthroughs**: CodeRabbit already generates high-level PR summaries. Keep your top-level review comment brief and focus strictly on **actionable inline findings**.
- **Prioritize high-severity defects**: Focus on correctness, memory/lifetime safety, threading violations, and protocol compliance. Do not post style or formatting nitpicks already enforced by `.clang-format`.

## What to Prioritize

1. **C++20 & `folly::coro` Lifetime Safety**:
   - Inspect coroutine suspension points (`co_await`, `co_yield`) for dangling references, raw pointers, or `this` captures that can outlive the owning `MoQSession`, `MoQRelay`, `MoQForwarder`, or `MoQCache` on session/track teardown or cancellation.
   - Check lambda captures scheduled onto `folly::Executor` / `folly::EventBase` for use-after-free hazards.

2. **Threading & Performance (`**/*.{h,cpp}`)**:
   - Read the Threading Model and Relay and Forwarding sections of `ARCHITECTURE.md` before judging threading.
   - moxygen avoids mutexes and gratuitous atomics—including `std::shared_ptr` and `std::weak_ptr` (whose atomic refcounts contend across cores). Flag any new lock, atomic, `shared_ptr`, or `weak_ptr` unless the change strictly requires it and justifies it.
   - **picoquic transport (`moxygen/openmoq/transport/pico/**`)**: openmoq-owned code; read `moxygen/openmoq/transport/pico/README.md` first. Each picoquic context is single-threaded, so flag picoquic or session state touched from another thread without going through that thread's executor.
   - Flag new helpers that duplicate `folly` or existing moxygen code.

3. **MOQT Spec Compliance (`moxygen/**/*.{h,cpp}`)**:
   - Wire behavior must hold for every version in `kSupportedVersions` (`moxygen/MoQVersions.h`). Changes to framing, setup parameters, control messages, or error codes must say which versions they apply to and stay correct on the others.
   - Version-dependent behavior belongs behind the helpers in `MoQVersions.h`, not ad hoc version comparisons.
   - Changes to `MoQSession`, `MoQRelaySession`, `MoQFramer`, or `MoQCodec` need tests in `moxygen/test` covering each version they affect.

4. **Fork & Downstream Consistency**:
   - **moqx (`https://github.com/openmoq/moqx`)** forks `MoQRelay` and `MoQCache` and builds on `MoQForwarder`. A behavior change in `moxygen/relay/**` or in the `Publisher`, `Subscriber`, or `TrackConsumer` interfaces should say whether moqx needs the same change.
   - Changes to upstream-owned code should go upstream when possible; the PR description should say why one lands here first (`.github/CONTRIBUTING.md`).
   - Workflows prefixed `omoq-` belong to this fork (`openmoq/README.md`); the others come from upstream. PR builds run without secrets.
   - `standalone/` is the fork's CMake build; `standalone/patches/<dep>/` patches third-party dependencies.

5. **Sync PRs** (`sync/<sha>` merges upstream into `main`; `sync-picoquic/<sha>` only moves the picoquic pin):
   - Summarize what upstream changed instead of reviewing upstream code for style.
   - The merge must not drop fork code. Flag removed fork-only behavior, flags, or tests (for example under `moxygen/openmoq/`, `standalone/`, or `omoq-` workflows) unless upstream now carries the same feature or fix.

## Paths & Files to Ignore

- Do **not** read or cite `pics/**`, `build/fbcode_builder/**`, or `.vscode/**`.
