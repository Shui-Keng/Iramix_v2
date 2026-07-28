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

## Plugin bridge

- bridge overhead target: below 5% of one core for 100 pass-through instances at
  48 kHz/128 samples on the reference desktop;
- no unbounded waits on a failed plugin process;
- scanner failure cannot terminate the main application;
- state snapshots have explicit size and time limits.
