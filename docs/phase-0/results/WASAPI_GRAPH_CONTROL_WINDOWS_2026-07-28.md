# WASAPI Graph Control Screening — Windows — 2026-07-28

## Purpose

This follow-up adds bounded parameter transport and exercises graph
replacement while the real WASAPI stream remains active. It extends, rather
than replaces,
[`WASAPI_GRAPH_INTEGRATION_WINDOWS_2026-07-27.md`](WASAPI_GRAPH_INTEGRATION_WINDOWS_2026-07-27.md).

## Implemented control path

The control-to-audio path now contains:

- a fixed-capacity SPSC queue with one control-thread producer and one
  audio-thread consumer;
- absolute sample-position timestamps;
- nondecreasing timestamp validation at enqueue;
- one preallocated parameter-event buffer per graph node;
- sample-offset routing at the beginning of each process block;
- explicit counters for full/rejected queue writes, late events, unknown node
  targets, and per-node buffer overflow;
- sample-accurate gain, pan, mute, and mixer-output events;
- queue reset as an explicit stopped-engine operation for seek/loop timeline
  discontinuities.

The executor queue capacity is 4,096 scheduled events. The WASAPI fixture
reserves 256 events per node per block. Reaching either limit rejects or drops
with a counter; neither storage layer grows in the callback.

## Deterministic tests

The standalone SPSC fixture verifies:

- exact bounded capacity;
- FIFO peek/pop behavior;
- rejection and counting of a write into a full queue.

The production-node graph schedules a gain change at absolute sample position
1,026 while rendering a four-frame block beginning at position 1,024. Samples
0–1 use the old value and samples 2–3 use the new value. An event subsequently
submitted at position 1,025 is rejected as out of order.

Raw test output:

```text
Production node chain: channels=2, frames=4, allocations=0, deallocations=0, blocking_locks=0, automation_offset=2
```

## Live WASAPI procedure

At the end of the 100-callback warmup:

1. The device thread signals a dedicated control thread.
2. The control thread prepares and publishes a replacement plan with a new
   gain-node instance.
3. The control thread waits for the audio thread to acknowledge generation 2.
4. It schedules a gain event two device blocks ahead.
5. It reclaims the acknowledged generation-1 plan.
6. The stream continues for the remainder of the 15-second measurement.

No callback waits for the control thread.

## Raw result

```text
audio_probe backend=WASAPI_shared_with_exclusive_fallback sample_rate=48000 seconds_per_buffer=15 callback_workload=immutable_graph_production_nodes
buffer=64 status=unsupported_device_period backend=WASAPI_exclusive stream_buffer=0 period_min=480 period_max=480 period_fundamental=480
buffer=128 status=unsupported_device_period backend=WASAPI_exclusive stream_buffer=0 period_min=480 period_max=480 period_fundamental=480
buffer=256 status=measured backend=WASAPI_exclusive stream_buffer=256 period_min=480 period_max=480 period_fundamental=480 callbacks=2813 p50_ms=0.064400 p95_ms=0.076200 p99_ms=0.087100 max_ms=0.303900 target_misses=0 hard_deadline_misses=0 late_wakeups=0 wait_timeouts=0 callback_allocations=0 callback_deallocations=0 callback_blocking_locks=0 graph_blocks=2913 graph_generation=2 graph_observed_swaps=2 reclaimed_plans=1 hot_swap=completed automation=queued parameter_pending=0 parameter_rejected=0 parameter_late=0 parameter_overflow=0 graph_control_failed=false mmcss=enabled
```

For the supported 256-frame stream:

- generation 2 was published and acknowledged;
- both generations were observed by the audio thread;
- the retired plan was reclaimed on the control thread;
- the parameter event was consumed;
- pending, rejected, late, and overflow counts ended at zero;
- p99 was `0.087100 ms` against the `3.73 ms` target;
- maximum callback work was `0.303900 ms`;
- target misses, hard deadline misses, and late wakeups were zero;
- callback allocations, deallocations, and blocking locks were zero.

The executable returned code 4 because this endpoint still rejected 64- and
128-frame requests. That failure remains visible and P0-008 stays open.

## Remaining limits

- This is one hot swap and one automation event, not a long-running edit
  storm.
- Parameters step at exact sample offsets; ramp generation and modulation-rate
  streams are not implemented.
- The producer is the native control thread, not yet a command decoded from
  the Java IPC session.
- Timeline seek and loop-wrap integration must explicitly flush/rebase queued
  absolute timestamps.
- Bounded audio telemetry was still missing in this run; it is implemented
  and measured in
  [`WASAPI_COMMAND_TELEMETRY_WINDOWS_2026-07-28.md`](WASAPI_COMMAND_TELEMETRY_WINDOWS_2026-07-28.md).
- ThreadSanitizer/AddressSanitizer evidence and the two-hour hardware soak are
  still required.
- ASIO, Core Audio, and Linux backends do not yet run this control path.

This completes the initial bounded parameter-queue and live-WASAPI hot-swap
slice without closing the full Week 5 or P0-008 gates.
