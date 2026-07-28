# Phase 0 Performance Budgets

These are engineering targets, not marketing claims. Reference machines and
sessions must be recorded beside every result.

## Audio

At 48 kHz:

| Buffer | Deadline | Engine p99 target |
|---|---:|---:|
| 64 samples | 1.33 ms | <= 0.93 ms |
| 128 samples | 2.67 ms | <= 1.87 ms |
| 256 samples | 5.33 ms | <= 3.73 ms |

The engine p99 target reserves 30% of the callback deadline for operating
system jitter and safety margin.

Additional gates:

- zero heap allocations in the callback after startup;
- zero blocking locks in the callback;
- zero engine-caused dropouts in a two-hour reference soak test;
- deterministic offline render mode;
- bounded behavior when event or telemetry queues are full.

The first Windows screening result is recorded in
[`results/AUDIO_CALLBACK_WINDOWS_2026-07-27.md`](results/AUDIO_CALLBACK_WINDOWS_2026-07-27.md).
Only the 256-frame configuration opened on the default endpoint; 64 and 128
frames were rejected by that driver.

The follow-up
[`ASIO screening`](results/AUDIO_CALLBACK_ASIO_WINDOWS_2026-07-27.md) opened
64, 128, and 256 frames. Callback execution p99 met all three targets, but the
64-frame run delivered only 87.416% of its nominal callback cadence. P0-008
therefore remains open, alongside the two-hour and cross-platform evidence.

The first
[`WASAPI immutable-graph integration screening`](results/WASAPI_GRAPH_INTEGRATION_WINDOWS_2026-07-27.md)
ran the production-node render plan through the real device callback. The
endpoint again rejected 64 and 128 frames. At 256 frames, the 15-second run
recorded p99 `0.080500 ms`, zero target/deadline misses, zero callback
allocations/deallocations/blocking locks, and one late wakeup. This is an
integration smoke result, not a replacement for the two-hour soak.

The
[`live graph-control follow-up`](results/WASAPI_GRAPH_CONTROL_WINDOWS_2026-07-28.md)
published generation 2 and consumed a sample-timestamped parameter event while
the 256-frame WASAPI stream remained active. Over 2,813 measured callbacks it
recorded p99 `0.087100 ms`, maximum `0.303900 ms`, zero target/deadline misses,
zero late wakeups, zero callback allocations/deallocations/blocking locks, and
zero pending/rejected/late/overflow parameter events at shutdown. The endpoint
again rejected 64 and 128 frames.

The
[`bounded command/telemetry follow-up`](results/WASAPI_COMMAND_TELEMETRY_WINDOWS_2026-07-28.md)
added completion backpressure and droppable block telemetry. The 15-second
256-frame stream consumed one applied command completion and 2,913 telemetry
records with zero completion loss or telemetry drops. It recorded p99
`0.018700 ms`, maximum `0.140700 ms`, and zero callback
allocations/deallocations/blocking locks. The endpoint still rejected 64 and
128 frames, so this does not close P0-008.

Core Audio and JACK initial runs plus the final two-hour per-OS sequence are
defined in [`AUDIO_PROBE_RUNBOOK.md`](AUDIO_PROBE_RUNBOOK.md). A compiled probe
without a real target-OS audio stream is not measurement evidence.

## UI and renderer

| Refresh target | Total frame | Iramix CPU target | GPU target |
|---|---:|---:|---:|
| 60 Hz | 16.67 ms | <= 5.0 ms | <= 8.0 ms |
| 120 Hz | 8.33 ms | <= 2.5 ms | <= 4.0 ms |

Reference arrangement:

- 200 visible tracks;
- 2,000 visible clips;
- waveform cache at three resolutions;
- 40 visible automation lanes;
- continuous horizontal pan and zoom;
- four animated meters per visible mixer channel.

No single UI interaction may synchronously perform filesystem indexing, plugin
scanning, waveform analysis, or project serialization.

Renderer correctness begins with a Java/Skiko reference scene and automated
startup/exit smoke test. Screenshot comparison is added after font shaping and
HiDPI reference assets are pinned.

## UI–engine control IPC

- local command round trip p99 target: below 10 ms under normal UI load;
- every persistent command has a sequence and explicit ACK or REJECT;
- client-side response timeout: five seconds during the Phase 0 spike;
- malformed or oversized frames terminate the probe cleanly;
- telemetry may be dropped, but persistent commands may not be silently lost.

The first Windows load-test recorded p50 0.094 ms, p95 0.247 ms, p99 1.807 ms,
and a 9.535 ms maximum over 1,000 measured sequential commands under dummy
Skia load. This is evidence for Windows only, not final acceptance of the
cross-platform target.

## Persistence and recovery

- acknowledged edit loss after crash: no more than five seconds;
- normal incremental save UI stall: below 16 ms;
- project open target: below five seconds for the reference large project;
- forced termination during recording must leave recoverable audio up to the
  last flushed block.

The initial
[`persistence/recovery foundation`](results/PERSISTENCE_RECOVERY_2026-07-28.md)
preserved committed snapshots across injected replacement failures, repaired a
partial command-journal tail, and recovered two durably flushed recording
blocks after a child process exited at an injected crash point. Two durable
journal appends took 6 ms in the local MSVC run. Large-file streaming,
reference-project open time, and the UI-stall budget remained unmeasured in
that initial slice.

The follow-up
[`bounded disk-audio worker screening`](results/DISK_AUDIO_WORKERS_2026-07-28.md)
validated a two-slot recording queue, two-slot playback read-ahead, explicit
recording rejection, and deterministic silence on playback underflow. The
allocation hook reported zero callback allocations/deallocations and zero
tracked blocking locks. A 4,243,472-byte, 2,048-block recording was validated
with a fixed 65,536-byte scanner scratch buffer and no sample materialization.
This is deterministic worker/format evidence, not device-stream dropout or
full reference-recording performance evidence.

The
[`async save and session migration screening`](results/ASYNC_SAVE_SESSION_MIGRATION_2026-07-28.md)
submitted 200 sequential immutable session revisions through an eight-slot
pipeline under active durable-save load. The local MSVC run measured submit
p50 `0.0005 ms`, p95 `0.0051 ms`, p99 `0.0061 ms`, and maximum `0.0147 ms`.
Every accepted revision received a committed completion, the final project
opened at revision 200, and an injected failure produced an explicit rejection.
This meets the submit-call portion of the below-16-ms UI-stall budget. It does
not include session serialization, IPC, or durable completion latency, so the
complete normal-save UI budget remains open.

The
[`reference session schema-v3 screening`](results/REFERENCE_SESSION_SCHEMA_V3_2026-07-28.md)
uses 200 tracks, 2,000 clips, 199 routes, 40 automation lanes, and 40,000
automation points. The 652,772-byte local MSVC Release project recorded
serialize p50 `3.4328 ms`, p95 `17.1632 ms`, p99/max `21.4607 ms`; combined
project-envelope load plus schema decode recorded p50 `14.7268 ms`, p95
`44.0381 ms`, and p99/max `61.7546 ms` over 20 iterations using nearest-rank
percentiles. This passes the five-second open target for the synthetic,
warm-local corpus. It is not final cold-cache/reference-hardware evidence, and
the serialization tail exceeding 16 ms confirms serialization must remain off
the UI thread.

The
[`background session-save and IPC screening`](results/BACKGROUND_SESSION_SAVE_IPC_2026-07-28.md)
moved validation and serialization onto the native save worker. Over 20
reference-session revisions, local MSVC Release immutable-snapshot submission
recorded p50 `0.0005 ms`, p95 `0.0012 ms`, and p99 `0.0014 ms`; worker
serialization recorded p50 `2.8118 ms`, p95 `3.7502 ms`, and p99 `3.8251 ms`;
durable save recorded p50 `13.5442 ms`, p95 `20.8262 ms`, and p99 `28.8445
ms`. Submission meets the below-16-ms UI-stall budget. Durable completion is
intentionally asynchronous and is not claimed to finish within one UI frame.
The Java/C++ save smoke supplies correctness evidence for one small project,
not IPC latency percentiles.

The
[`production-session and save-coalescing screening`](results/PRODUCTION_SESSION_SAVE_COALESCING_2026-07-28.md)
deep-copied 20 immutable snapshots of the 200-track/2,000-clip/40,000-point
reference document. Local MSVC Release recorded snapshot p50 `0.28 ms`, p95
`0.6029 ms`, and p99/max `1.1254 ms`. A deterministic revision 2/3/4 burst
submitted only revisions 2 and 4 while preserving revision 4 as the durable
superset. Snapshot creation is below the 16 ms UI-stall target in this warm
local screening; cold reference-hardware evidence remains open.

The
[`journaled session and undo/redo screening`](results/JOURNALED_SESSION_UNDO_REDO_2026-07-28.md)
measured 100 sequential, durably flushed tempo edits in local MSVC Release:
p50 `2.6163 ms`, p95 `4.0549 ms`, p99 `10.1046 ms`, and maximum `12.2645
ms`. The p99 exceeds the provisional below-10-ms UI-engine command target by
`0.1046 ms` before Java transport or dummy UI load, so the performance gate is
explicitly not met by this run. Correctness/recovery tests pass; group commit,
persistent journal handles, and end-to-end loaded measurement remain follow-up
work.

The
[`autosave/checkpoint screening`](results/AUTOSAVE_CHECKPOINT_COMPACTION_2026-07-28.md)
coalesced three edits behind a 30 ms fixed window and durably saved revision 4
in `56.9689 ms` from the first dirty mark on the local MSVC Release run. One
autosave request covered two dirty-snapshot replacements; slower GCC runs
legitimately used two fixed windows rather than starving the deadline.
Checkpointing reduced an eight-record, 800-byte journal to two records and 200
bytes in one raw `4.8606 ms` sample, then reconstructed active undo and redo
after reopen. These are short synthetic screenings, not reference-storage or
power-loss evidence.

## Plugin bridge

- bridge overhead target: below 5% of one core for 100 pass-through instances at
  48 kHz/128 samples on the reference desktop;
- no unbounded waits on a failed plugin process;
- scanner failure cannot terminate the main application;
- state snapshots have explicit size and time limits.
