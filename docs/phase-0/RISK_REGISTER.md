# Phase 0 Risk Register

Scoring: probability and impact range from 1 to 5. Priority is their product.

| ID | Risk | P | I | Priority | Phase 0 response |
|---|---|---:|---:|---:|---|
| R-01 | Scope exceeds available team capacity | 5 | 5 | 25 | Enforce the v1 scope contract and validate team size |
| R-02 | Plugin crash or hang destabilizes audio | 4 | 5 | 20 | Bounded-deadline shared-memory bridge measured: host survives child crash and hang, degrading to counted silence. See `results/PLUGIN_PROCESS_ISOLATION_2026-07-28.md`. Isolation and real VST3 hosting are now joined: the bridge child can host a real plugin (`results/VST3_HOSTING_IN_BRIDGE_CHILD_2026-07-29.md`), three third-party plugins verified, 532/532 blocks each. That work added a third reason for isolation beyond crash and hang: a Pure Data based plugin writes its banner to **stdout**, which is the transport P0-007 uses for the Java/C++ boundary, so an in-process plugin would corrupt the protocol rather than crash it. Crash/hang recovery is now also proven against real plugins: `results/VST3_CRASH_HANG_RECOVERY_2026-07-29.md` injects the same fault after three genuine blocks of real DSP, against two third-party plugins, both fault kinds. `testPluginProcessTermination`/`testPluginProcessHang` still cover the stand-in only in `ctest`, since the real-plugin variant depends on a locally installed plugin no CI runner has |
| R-03 | Skiko/Skia GPU behavior differs substantially by OS | 4 | 4 | 16 | Run one renderer and device-loss spike per OS. **Partially open.** Slice 1 (`results/SKIA_GPU_BACKEND_FRAMETIME_2026-07-29.md`) landed a backend matrix across all three CI OSes: Direct3D on Windows, Metal on macOS, and Skiko's software fallback under Xvfb on Ubuntu CI. The Windows reference-machine median sits at the 60 Hz budget line (16.6ms) and misses 120 Hz; p99 widens from 28ms isolated to 67ms under `gradle check`. Slice 2 (`results/SKIA_GPU_RESIZE_MONITOR_RECOVERY_2026-07-29.md`) covers resize locally and in hosted CI: a continuously rendering window completed nine sizes/eight transitions with no crash, exception, timeout, or hang on Direct3D, Metal, and Ubuntu/Xvfb software. **Monitor movement is not done:** every available environment exposes one `GraphicsDevice`, and the task reports `LIMITATION_SINGLE_MONITOR`. Slice 3 (`results/SKIA_GPU_CONTEXT_RECREATION_PROXY_2026-07-29.md`) disposes/reinitializes the same SkiaLayer backend five times locally; each new Direct3D context initializes and resumes completed presents. It is explicitly a `CONTEXT_RECREATE_PROXY`. **Literal OS sleep/wake is accepted as a Phase 0 evidence gap**, because safely suspending and resuming the host cannot be automated by a normal CI/test process; see "Literal sleep/wake coverage gap." A real Linux GPU backend, cross-monitor/mixed-DPI recovery, and device loss are still unmeasured |
| R-04 | Linux plugin editors fail under Wayland | 4 | 4 | 16 | **Cannot be closed in Phase 0: no Linux hardware (see R-13).** Native Wayland has no cross-client reparenting mechanism at all, so the generic-editor fallback is a requirement rather than a contingency. See `results/PLUGIN_EDITOR_EMBEDDING_RISK_2026-07-29.md` |
| R-05 | Multicore graph scheduling misses deadlines | 3 | 5 | 15 | Stabilize single coordinator first; benchmark graph partitions |
| R-06 | Project corruption destroys user work | 2 | 5 | 10 | Journaled commands, atomic saves, and forced-crash drills |
| R-07 | AWT/Skiko lacks DAW-grade input, accessibility, or native-window behavior | 4 | 3 | 12 | Test IME, accessibility, HiDPI, focus, and plugin-window embedding early. One sub-item has evidence: text shaping and font fallback are measured on all three operating systems in `results/SKIA_RASTER_BASELINE_2026-07-29.md`, and the finding is adverse — Skia's single-font shaping path silently produces `.notdef` for uncovered scripts while still reporting a plausible advance width, so **font fallback is application work Iramix must implement, not a toolkit service**. Coverage is not portable either: Arabic renders on Windows and Linux but is 9/10 `.notdef` on macOS's Helvetica Neue, and identical ASCII measures 133.01 on Windows against 153.40 on Linux, so **measured text extents must never be hard-coded**. Input, IME, accessibility, focus, and real HiDPI surfaces remain untouched |
| R-08 | JVM/Skiko packaging size and runtime update cost slow delivery | 4 | 3 | 12 | Pin dependencies and produce reproducible per-OS runtime images |
| R-09 | ASIO proprietary developer agreement is not yet signed | 1 | 2 | 2 | Administrative action: register through the Steinberg Developer Portal before ASIO enters a public release build |
| R-10 | Custom UI consumes capacity needed by audio engine | 4 | 4 | 16 | Limit widget set and measure delivery velocity at week 4 |
| R-11 | Cross-platform behavior drifts | 4 | 3 | 12 | Three-OS CI plus screenshot and project round-trip tests. Screenshot comparison now runs in `gradle check` on all three CI operating systems and **guards all three**: Skia CPU raster output is measured bit-identical on `windows-x64`, `macos-arm64`, and `linux-x64` — across x86-64 and aarch64 — so the same baselines are committed for each. See `results/SKIA_RASTER_BASELINE_2026-07-29.md`. Impact lowered from 4 to 3 for rendering drift specifically, because drift is now *detected* rather than merely feared. The risk stays open: the identity is observed and not explained, so it must be re-checked on any Skiko upgrade, and text is explicitly excluded — advance widths differ by up to 15% between platforms and script coverage differs outright |
| R-12 | Plugin state blocks autosave or recovery | 3 | 4 | 12 | Size and time limits measured: the state region is sized before audio flows, an oversized blob is refused before it is written, and a capture from a crashed plugin times out at its deadline rather than stalling. See `results/PLUGIN_STATE_RESTORATION_2026-07-28.md`. Capture is now wired into a real `AutosaveClock`-scheduled autosave window and read back through the project store: see `results/PLUGIN_STATE_AUTOSAVE_WIRING_2026-07-29.md`. Still open: only the stand-in plugin and a single successful binding were exercised — a capture timeout or rejection arriving exactly when an autosave window fires is untested, and no `SessionController`/`JournaledSession` caller exists yet to invoke the new capture-into-autosave path during real editing |
| R-13 | No macOS or Linux hardware is available to the project | 5 | 4 | 20 | **Accepted for Phase 0.** Hosted three-OS CI covers portability, correctness, and sanitizers; performance evidence is Windows-only by decision. See "Reference-hardware coverage gap" |
| R-14 | Redistributing third-party binaries without required attribution | 2 | 4 | 8 | Skiko runtime jars ship Skia and ICU binaries with no notice file; obligations L-1 to L-4 tracked in `DEPENDENCIES.md` and gated before any distributable build |

## Critical escalation rule

Any risk with priority 16 or above must have measured evidence or an accepted
fallback before Phase 1 begins.

## Reference-hardware coverage gap

**Status: accepted as a Phase 0 risk on 2026-07-28.**

R-13 is a hardware constraint, not an open engineering task: the project has
no physical macOS or Linux machine. Hosted CI proves the code builds, passes
tests, and is clean under ASan/UBSan and TSan on all three operating systems,
but shared runners cannot produce cold-cache open timings or any other
reference-hardware performance figure — their storage and cache behavior is
neither controlled nor representative.

### Accepted position

Phase 0 performance evidence is **Windows-only**, deliberately. The
reasoning, recorded here so a later reader does not mistake it for an
oversight:

1. No macOS or Linux hardware is accessible to the project, so the
   measurement cannot be taken at all — this is not a matter of effort or
   scheduling.
2. Hosted CI runners are sufficient evidence for portability, correctness,
   and sanitizer cleanliness, and are used as such throughout Phase 0.
3. Hosted CI runners are **not** evidence of performance, and no timing
   figure may be quoted from them. The build matrix runs `ctest` without
   `-V`, so benchmark output does not even reach the logs, and the Windows
   leg builds Debug rather than Release.

Consequently every result document carrying a timing figure must state the
platform and toolchain explicitly and must list macOS and Linux performance
under its evidence boundary.

### What this defers

Cold-cache open behavior, storage-contention behavior, and audio-callback
deadline behavior on macOS and Linux remain unmeasured entering Phase 1.
Closing them requires borrowed, rented, or purchased hardware on both
platforms; it cannot be closed by additional work on the existing machine.
This acceptance covers Phase 0 only and should be revisited when Phase 1
scope is committed.

## Literal sleep/wake coverage gap

**Status: accepted as a Phase 0 evidence gap on 2026-07-29.**

R-03 calls for renderer recovery across sleep/wake, but an ordinary test
cannot safely suspend the operating system that owns the test process,
its timeout, its logs, and its CI agent. Hosted runners do not expose a
contract for guest-driven host suspend/resume, and forcing sleep on a
development machine from `gradle check` could interrupt unrelated work or
leave the run unobservable.

### Accepted position

Phase 0 uses deterministic Skiko context disposal/recreation as a bounded
**proxy**, and labels it as such in both code output and the result
document. Literal OS sleep/wake is not inferred from that proxy and is not
marked done.

Closing the literal gap requires a controlled physical-machine run with
an out-of-band wake mechanism and logging that survives the interruption.
That manual hardware exercise is deferred beyond automated Phase 0
gating. This acceptance does not cover device loss, monitor movement, or
mixed-DPI recovery; those retain their own evidence boundaries.

## ASIO administrative and trademark note

The licensing path is resolved: Iramix selected Steinberg's free proprietary
ASIO SDK license, with no source-disclosure obligation. R-09 is no longer a
technical or licensing-model blocker; it tracks completion of the developer
agreement before public distribution.

Use of the ASIO name or logo is optional. If either is used, Steinberg's
trademark and usage rules apply. The initial product policy is to support the
protocol without ASIO branding or logo, avoiding an additional trademark work
stream during the early release stages.

## Project-corruption evidence

R-06 has initial mitigation evidence in
[`results/PERSISTENCE_RECOVERY_2026-07-28.md`](results/PERSISTENCE_RECOVERY_2026-07-28.md):
atomic snapshot failure injection, journal-tail repair, and forced-process-exit
recording recovery pass on Windows, macOS, Linux, ASan/UBSan, and TSan. The
risk is further reduced by fixed-memory recording scans, invalid-tail
truncation, and bounded recording/read-ahead queues with explicit pressure
behavior. It remains open until full-scale recording runs, full session round
trips, migration drills, and reference-project benchmarks pass. The first
stable-ID session round trip, v1-to-v2 migration fixture, bounded async-save
pipeline, and failure-to-rejection ordering are now covered by
[`ASYNC_SAVE_SESSION_MIGRATION_2026-07-28.md`](results/ASYNC_SAVE_SESSION_MIGRATION_2026-07-28.md).
Schema-v3 clip/routing/automation references, v1/v2 migrations, lossy-export
rejection, and the first synthetic reference-session benchmark are covered by
[`REFERENCE_SESSION_SCHEMA_V3_2026-07-28.md`](results/REFERENCE_SESSION_SCHEMA_V3_2026-07-28.md).
Background session serialization, revisioned Java/C++ save acceptance,
ordered durable completion, explicit saturation, and separate serialization
and save-worker timings are covered by
[`BACKGROUND_SESSION_SAVE_IPC_2026-07-28.md`](results/BACKGROUND_SESSION_SAVE_IPC_2026-07-28.md).
Native session ownership, revision conflicts, immutable snapshot isolation,
stable-ID continuation, and latest-revision save coalescing are covered by
[`PRODUCTION_SESSION_SAVE_COALESCING_2026-07-28.md`](results/PRODUCTION_SESSION_SAVE_COALESCING_2026-07-28.md).
Write-ahead live edits, monotonic undo/redo, snapshot-plus-journal recovery,
history reconstruction, and stable-ID redo are covered locally by
[`JOURNALED_SESSION_UNDO_REDO_2026-07-28.md`](results/JOURNALED_SESSION_UNDO_REDO_2026-07-28.md).
The production-session and initial journal/undo portions are therefore
addressed, including hosted three-OS, Java 21, ASan/UBSan, and TSan
confirmation. Fixed-window autosave, orderly-shutdown flush, and
revision-gated history compaction now have local evidence in
[`AUTOSAVE_CHECKPOINT_COMPACTION_2026-07-28.md`](results/AUTOSAVE_CHECKPOINT_COMPACTION_2026-07-28.md).
Hosted Windows, macOS, Linux, Java 21, ASan/UBSan, and TSan confirmation for
that slice is green. Revisioned backup creation, bounded retention,
unknown-file preservation, corrupt-envelope skipping, and primary/backup
failure isolation now have local evidence in
[`PROJECT_BACKUP_ROTATION_2026-07-28.md`](results/PROJECT_BACKUP_ROTATION_2026-07-28.md).
The backup slice is also green on hosted Windows, macOS, Linux, ASan/UBSan,
and TSan. Fail-closed automatic restore, embedded-revision checks, explicit
journal checkpoint baselines, journal replay, and atomic active-project
replacement now have local evidence in
[`AUTOMATIC_BACKUP_RESTORE_2026-07-28.md`](results/AUTOMATIC_BACKUP_RESTORE_2026-07-28.md).
That restore slice is also green on hosted Windows, macOS, Linux,
ASan/UBSan, and TSan. R-06 remains open for complete state, cold storage, and
physical power-loss evidence. The first
durable-command latency p99 also missed the provisional 10 ms control target,
so performance optimization must not be treated as complete.
