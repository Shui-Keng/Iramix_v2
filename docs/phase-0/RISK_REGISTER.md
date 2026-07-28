# Phase 0 Risk Register

Scoring: probability and impact range from 1 to 5. Priority is their product.

| ID | Risk | P | I | Priority | Phase 0 response |
|---|---|---:|---:|---:|---|
| R-01 | Scope exceeds available team capacity | 5 | 5 | 25 | Enforce the v1 scope contract and validate team size |
| R-02 | Plugin crash or hang destabilizes audio | 4 | 5 | 20 | Prototype process isolation and bounded deadlines |
| R-03 | Skiko/Skia GPU behavior differs substantially by OS | 4 | 4 | 16 | Run one renderer and device-loss spike per OS |
| R-04 | Linux plugin editors fail under Wayland | 4 | 4 | 16 | Test XWayland, native Wayland, and generic editor fallback |
| R-05 | Multicore graph scheduling misses deadlines | 3 | 5 | 15 | Stabilize single coordinator first; benchmark graph partitions |
| R-06 | Project corruption destroys user work | 2 | 5 | 10 | Journaled commands, atomic saves, and forced-crash drills |
| R-07 | AWT/Skiko lacks DAW-grade input, accessibility, or native-window behavior | 4 | 3 | 12 | Test IME, accessibility, HiDPI, focus, and plugin-window embedding early |
| R-08 | JVM/Skiko packaging size and runtime update cost slow delivery | 4 | 3 | 12 | Pin dependencies and produce reproducible per-OS runtime images |
| R-09 | ASIO proprietary developer agreement is not yet signed | 1 | 2 | 2 | Administrative action: register through the Steinberg Developer Portal before ASIO enters a public release build |
| R-10 | Custom UI consumes capacity needed by audio engine | 4 | 4 | 16 | Limit widget set and measure delivery velocity at week 4 |
| R-11 | Cross-platform behavior drifts | 4 | 4 | 16 | Three-OS CI plus screenshot and project round-trip tests |
| R-12 | Plugin state blocks autosave or recovery | 3 | 4 | 12 | Snapshot asynchronously with size/time limits |

## Critical escalation rule

Any risk with priority 16 or above must have measured evidence or an accepted
fallback before Phase 1 begins.

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
confirmation. R-06 remains open for complete state, autosave and journal
compaction, cold storage, backup, and physical power-loss evidence. The first
durable-command latency p99 also missed the provisional 10 ms control target,
so performance optimization must not be treated as complete.
