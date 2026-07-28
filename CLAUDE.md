# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project state: Phase 0

Iramix is a cross-platform DAW (Java 21 + Skiko/Skia UI process, C++20
real-time engine, no JUCE). It is in **Phase 0: technical validation**.
Production features are deliberately not built until the gates in
[`docs/phase-0/PLAN.md`](docs/phase-0/PLAN.md) are met.

This changes how work is judged here: a spike is not done when it runs, it
is done when it has **measured evidence and a stated evidence boundary**.
See "Evidence discipline" below — it is the single most important
convention in this repository.

## Build and test

Presets: `dev` (Debug+Ninja), `release`, `windows-msvc` (Visual Studio,
works without a compiler shell), `sanitizers` (ASan+UBSan, Linux-only
condition), `thread-sanitizer` (Linux-only condition).

```bash
cmake --preset dev && cmake --build --preset dev && ctest --preset dev
```

Run one test target, with its stdout (needed to read the counter lines):

```bash
ctest --preset dev -R iramix.persistence -V
```

Test names: `iramix.smoke`, `iramix.audio_graph`, `iramix.plan_swap`,
`iramix.persistence`, `iramix.session`, `iramix.audio_probe.audit`.
Test binaries can also be run directly (`build/<preset>/iramix_*_tests`),
which is the fastest way to iterate on one assertion.

Java UI and the Java↔C++ handshake (pinned toolchain via `scripts/`):

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\gradle.ps1 check
```

Full Windows Phase 0 verification including a live IPC session:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\verify-phase0.ps1
```

CI (`.github/workflows/ci.yml`) runs on every push: build+`ctest`+`gradle
check` on Windows/macOS/Ubuntu, plus ASan/UBSan and TSan on Ubuntu. Note
CI runs `ctest` **without** `-V`, so no benchmark output reaches the logs —
CI proves portability and sanitizer cleanliness, never performance.

## Evidence discipline

Every validated spike produces a dated result document in
`docs/phase-0/results/` and a status update to the task board table in
`docs/phase-0/PLAN.md`. Follow the existing documents exactly:

- **Raw counters, not adjectives.** Each test prints one machine-readable
  summary line to stdout (e.g. `Session document: current_schema=4,
  tracks=3, ...`). That literal line is pasted into the result document.
  When you add a behavior, add its counter to that line.
- **Percentiles are nearest-rank**, and with 20 samples p99 *is* the
  observed maximum. Say so rather than implying more resolution.
- **Every document ends with an "Evidence boundary" section** listing what
  the result does *not* prove. This is not boilerplate — it is how the
  project avoids over-claiming. Be specific and unflattering: name the
  untested platform, the unverified metadata, the absent decoder.
- **State the toolchain and build type** with any timing figure. Numbers
  from different compilers are not comparable and must not be presented as
  a regression comparison.
- Risks live in `docs/phase-0/RISK_REGISTER.md`; anything at priority 16+
  needs evidence or an accepted fallback before Phase 1. A blocker that
  engineering cannot resolve (e.g. missing hardware) belongs there as a
  risk, not in a plan as a perpetually pending task.

## Architecture

Two processes, three independently versioned schemas (project/session
format, command journal, IPC protocol). Read
[`docs/architecture/SYSTEM_OVERVIEW.md`](docs/architecture/SYSTEM_OVERVIEW.md)
and the ADRs in `docs/adr/` before changing a boundary; ADR-0003
(immutable render plans) and ADR-0004 (transactional persistence) are the
two that constrain most engine work.

### Real-time engine (`src/audio/`, `include/iramix/audio/`)

The editable session and the executable graph are **separate models**. The
graph compiler produces an immutable `RenderPlan`; `RenderPlanExecutor`
loads one atomic plan pointer per block. A prepared plan co-owns its
nodes, so replacing topology cannot invalidate nodes the callback is still
using; reclamation is acknowledgement-based and happens off the audio
thread.

The audio callback performs **no allocation, locking, filesystem access,
logging, or destruction**. Control→audio traffic uses bounded lock-free
SPSC queues with absolute sample timestamps; saturation, late events, and
overflow are explicit counters rather than silent drops.
`apps/audio-probe/AllocationHooks.cpp` instruments this, and the graph
tests assert it.

### Session and persistence (`src/persistence/`, `src/session/`)

Layered, each stage owning one guarantee:

- `SessionDocument` — the persistence DTO plus its versioned binary
  schema. Validates on both serialize and deserialize.
- `SessionController` — control-thread-owned editable session; every edit
  carries an expected revision, stale/invalid edits leave the document
  unchanged, and stable IDs are allocated monotonically.
- `JournaledSession` — write-ahead boundary. Applies to a *candidate*
  document, durably appends the forward/inverse command pair, and only
  then publishes and permits an ACK. Undo/redo append new monotonic
  revisions rather than rewinding the counter. An ambiguous append failure
  poisons the edit path until reopen.
- `SessionPersistenceService` / `SessionSaveCoordinator` /
  `AsyncSessionSaver` — fixed autosave window anchored to the first dirty
  revision, coalescing toward the newest revision, shutdown flush.
  Validation and serialization happen on the save worker, never on the
  Java UI or command-dispatch thread. The autosave deadline runs on an
  injected `AutosaveClock`: production uses `SteadyAutosaveClock`, tests
  use `ManualAutosaveClock` and step virtual time. Do not reintroduce
  `sleep_for` to cross that window — real coordinator I/O is still awaited
  in real time, but the deadline itself must stay deterministic.
- `Persistence.cpp` / `ProjectStore` / `ProjectBackupStore` — checksummed
  envelopes, durable sibling staging file, atomic replace, revisioned
  backup rotation, fail-closed automatic restore.
- `MediaResolver` — relinks external media by bounded content hash;
  rewrites paths for verified relocations only.
- `DeviceResolver` — decides what to open from a stored device
  configuration against an *injected* device inventory. Both resolvers are
  pure logic over supplied inputs rather than direct filesystem/hardware
  callers, which is what makes restoration testable without the media or
  hardware present; keep new restoration layers in that shape.

### Schema evolution rules

When extending `SessionDocument`:

1. Bump `currentSessionSchemaVersion` and gate the new block on it in both
   `serializeSessionDocument` and `deserializeSessionDocument`.
2. Migration from older schemas must be **deterministic** and fail closed —
   never guess a reference. Synthesizing a placeholder is acceptable only
   when the placeholder is honest about being unresolved.
3. Export to an older schema must be **rejected** whenever it would
   discard data the older format cannot express. Only genuinely
   recoverable records may be dropped.
4. Add counts and per-entity bounds for anything unbounded, and enforce
   them on decode as well as encode — decode runs on untrusted bytes.
5. Extend the reference workload in `makeReferenceSession()` (it exists in
   both `tests/PersistenceTests.cpp` and `tests/SessionTests.cpp`) and
   update the declared scale in the result document.

## Code conventions

- C++20, no exceptions across API surfaces: report failures through a
  `std::string& error` out-parameter or a result struct with an `ok()`.
  `std::bad_alloc` is caught at the boundary and converted to an error.
- `[[nodiscard]]` on anything returning a status or a value the caller
  must inspect. `noexcept` where genuinely non-throwing.
- Sized integer types (`std::uint64_t`), `U`/`F` literal suffixes, digit
  separators (`48'000U`), `struct ... final` with default member
  initializers, designated initializers at call sites.
- 4-space indent, LF, ~72-column wrapping (see `.editorconfig`). Warnings
  are `/W4` or `-Wall -Wextra`; keep the build warning-free even though
  `-Werror` is not set. Fully initialize designated-initializer aggregates
  or use a named local — partial designated init trips
  `-Wmissing-field-initializers`.
- Comments explain *why a rule exists* (invariant, failure mode), not what
  the line does. Most existing comments mark a safety property; match that
  bar or omit the comment.

### Portability

The local toolchain is GCC/MinGW (MSYS2 UCRT64); CI additionally builds
MSVC, AppleClang, and Linux GCC. **A green local build proves very little
about portability** — this has already bitten twice:

- `std::uintmax_t` is `unsigned long` on libc++ but `unsigned long long`
  here, so `std::min(fileSize, someUint64)` compiled on Windows and Linux
  and failed to deduce on macOS. Convert explicitly.
- Windows SDK headers are order-sensitive and MinGW is far more forgiving
  than MSVC. `windows.h` comes first, not alphabetically.

When a construct's portability cannot be verified from this machine,
prefer the form that does not depend on the unverifiable assumption.
`WasapiProbe.cpp` defines `PKEY_Device_FriendlyName` locally rather than
relying on both a header include order *and* whichever import library a
toolchain happens to supply the symbol from.

## Tests

There is **no test framework**. Each `tests/*.cpp` is a `main()` that calls
`void testX(const std::filesystem::path& root)` functions in order, using a
hand-rolled `require(condition, message)` that prints and `std::exit`s.
New tests are registered by adding the call to `main()`.

Consequences worth knowing:

- Assertions are cheap to add; a single `require` with a compound
  condition is the house style.
- The counter line each test prints at the end is a deliverable, not
  debug output — it feeds the result document.
- Filesystem tests use the `TemporaryDirectory` RAII helper and take a
  root path; never write outside it.
- Negative cases matter as much as positive ones. The existing suites
  assert that invalid input is *rejected* (dangling references, duplicate
  IDs, truncated input, trailing bytes, lossy export), and new format work
  is expected to do the same.

## graphify

This project has a knowledge graph at graphify-out/ with god nodes, community structure, and cross-file relationships.

Rules:
- For codebase questions, first run `graphify query "<question>"` when graphify-out/graph.json exists. Use `graphify path "<A>" "<B>"` for relationships and `graphify explain "<concept>"` for focused concepts. These return a scoped subgraph, usually much smaller than GRAPH_REPORT.md or raw grep output.
- If graphify-out/wiki/index.md exists, use it for broad navigation instead of raw source browsing.
- Read graphify-out/GRAPH_REPORT.md only for broad architecture review or when query/path/explain do not surface enough context.
- After modifying code, run `graphify update .` to keep the graph current (AST-only, no API cost).
