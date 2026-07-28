# WASAPI Command and Telemetry Screening — Windows — 2026-07-28

## Purpose

This screening extends the immutable graph control path with bounded
general commands, explicit audio-thread completions, and droppable block
telemetry. It does not close P0-008 or the full Week 5 gate.

## Implemented queue contract

- Control-to-audio command queue: exact capacity 1,024, SPSC, monotonic
  nonzero sequence numbers, and explicit enqueue rejection.
- Audio-to-control completion queue: exact capacity 1,024. A full completion
  queue stops command draining, leaving accepted commands pending until the
  control thread consumes an ACK or REJECT; completions are not silently
  discarded.
- Audio-to-control telemetry queue: exact capacity 2,048. Telemetry is
  intentionally droppable and every full-queue rejection increments a
  counter.
- Callback command work: at most 64 commands per block.
- Implemented commands: reset one node and reset all nodes.
- Completion payload: sequence, status, target node, and applied render-plan
  generation.
- Telemetry payload: generation, absolute block sample position, frame count,
  applied/rejected command counts, and routed parameter-event count.

All queue elements are trivially copyable. Push, pop, capacity checks, and
callback command execution allocate no heap memory and take no blocking lock.

## Deterministic saturation evidence

The graph tests verify:

- two successful completions: one applied reset and one unknown-target REJECT;
- duplicate command sequence rejection;
- exact command-queue saturation and counted rejection;
- 1,024 unconsumed completions followed by completion backpressure;
- the next accepted command remains pending rather than losing its ACK;
- command processing resumes after the control side consumes one completion;
- telemetry saturation remains bounded and increments its drop counter;
- allocation, deallocation, and blocking-lock audit counts remain zero.

Release test output:

```text
Graph render audit: allocations=0, deallocations=0, blocking_locks=0, pdc_peak_frame=3, pdc_peak=2
Production node chain: channels=2, frames=4, allocations=0, deallocations=0, blocking_locks=0, automation_offset=2
All Iramix audio graph tests passed.
Plan swap stress: publications=1001, blocks=670163, observed_swaps=856, reclaimed=1000, retired=0, allocations=0, deallocations=0, blocking_locks=0, use_after_free=0, telemetry_dropped=668115
```

MSVC Debug, GCC/Ninja Debug, and GCC/Ninja Release each passed all four CTest
targets.

## Live WASAPI procedure

After the 100-block warmup, the native control thread:

1. publishes and waits for acknowledgement of render-plan generation 2;
2. queues sequence 1 to reset the mixer node;
3. consumes block telemetry while waiting for its completion;
4. verifies an `applied` completion for the mixer and generation 2;
5. queues the existing sample-timestamped gain automation event;
6. reclaims the retired generation-1 plan;
7. continues consuming telemetry until the stream stops.

The audio callback never waits for the control thread.

## Environment

- OS: Windows 11 Home Single Language, build 26200
- CPU: AMD Athlon Silver 3050U, 2 cores / 2 logical processors
- Build: MSVC x64 Release
- Sample rate: 48 kHz
- Duration: 15 seconds per requested buffer size

## Raw result

```text
audio_probe backend=WASAPI_shared_with_exclusive_fallback sample_rate=48000 seconds_per_buffer=15 callback_workload=immutable_graph_production_nodes
buffer=64 status=unsupported_device_period backend=WASAPI_exclusive stream_buffer=0 period_min=480 period_max=480 period_fundamental=480
buffer=128 status=unsupported_device_period backend=WASAPI_exclusive stream_buffer=0 period_min=480 period_max=480 period_fundamental=480
buffer=256 status=measured backend=WASAPI_exclusive stream_buffer=256 period_min=480 period_max=480 period_fundamental=480 callbacks=2813 p50_ms=0.010600 p95_ms=0.012600 p99_ms=0.018700 max_ms=0.140700 target_misses=0 hard_deadline_misses=0 late_wakeups=0 wait_timeouts=0 callback_allocations=0 callback_deallocations=0 callback_blocking_locks=0 graph_blocks=2913 graph_generation=2 graph_observed_swaps=2 reclaimed_plans=1 hot_swap=completed automation=queued parameter_pending=0 parameter_rejected=0 parameter_late=0 parameter_overflow=0 reset_command=queued reset_completion=applied command_pending=0 command_rejected=0 completion_lost=0 telemetry_received=2913 telemetry_dropped=0 graph_control_failed=false mmcss=enabled
```

For the supported 256-frame stream, command completion and all 2,913 block
telemetry records were consumed with no completion loss or telemetry drop.
Callback p99 was `0.018700 ms` against the `3.73 ms` engine target. Maximum
callback work was `0.140700 ms`; target misses, hard-deadline misses, late
wakeups, allocations, deallocations, and blocking locks were all zero.

The executable returned code 4 because this endpoint rejected the 64- and
128-frame configurations. Those configurations remain unsupported on this
device and are not counted as passing measurements.

## Remaining limits

- The live command originates on the native control thread; Java IPC decoding
  is not yet connected to this queue.
- The general command vocabulary currently covers reset operations only.
- Telemetry is a compact per-block proof schema, not yet meters, profiler
  streams, or UI snapshot aggregation.
- Parameter ramps, modulation-rate streams, denormal instrumentation,
  sanitizer evidence, edit-storm testing, and the two-hour hardware soak
  remain pending.
- ASIO, Core Audio, and Linux backends have not run this command path.

This closes the initial bounded command/completion/telemetry implementation
slice of P0-011 on Windows, while Phase 0, P0-008, and the Week 5 exit gate
remain open.
