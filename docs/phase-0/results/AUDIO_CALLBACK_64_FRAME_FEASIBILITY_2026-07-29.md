# Windows 64-Frame Buffer Feasibility — 2026-07-29

Status: P0-008 evidence. The 64-frame configuration is **not achievable on
this reference machine** on either Windows backend, for two different and
separately measured reasons. P0-008 remains open.

This document answers the first item the 2026-07-27 ASIO screening left
pending — "investigate or reproduce the 64-frame cadence deficit" — and
supersedes nothing: the earlier callback-duration figures still stand.

## What this slice changed in the probes

1. `StereoGraphWorkload` moved out of `WasapiProbe.cpp` into
   `apps/audio-probe/GraphWorkload.hpp`, and `AsioProbe.cpp` now measures
   it. Until now ASIO measured a stereo gain over silence plus two
   `memset` calls, while WASAPI measured the production node chain
   (`DeviceInput → Track → Gain → Mixer → Output` through
   `RenderPlanExecutor`). Two backends measuring different work cannot be
   compared, and the 64-frame question is exactly a cross-backend
   comparison. Both header lines now carry
   `callback_workload=immutable_graph_production_nodes`.
2. The ASIO probe converts the rendered planar float block into the
   driver's native sample format. Sample types it cannot write
   (every MSB variant, and the right-aligned `Int32LSBnn` family) are
   refused before `ASIOStart` rather than filled with wrong samples.
3. The ASIO probe drains the bounded telemetry queue on its control
   thread. Without a consumer every block past the queue depth counted as
   a drop — a 10-second trial reported `telemetry_dropped=4430` — which
   made the counter say nothing about whether the engine lost anything.
   Both backends now report `telemetry_dropped=0`.

The WASAPI refactor is behavior-preserving: its result line, counters and
status values are unchanged.

## Reference environment

- OS: Windows 11 Home Single Language 64-bit, build 26200.
- CPU: AMD Athlon Silver 3050U with Radeon Graphics, 2 cores / 2 threads.
- Memory: 21.9 GiB.
- Power: on AC, "High performance" power scheme.
- Build: **MSVC Release** (`windows-msvc` preset, `Release` configuration)
  for both backends. The 2026-07-27 ASIO screening was also MSVC Release,
  so the cadence figures below are comparable with it; nothing here is
  comparable with a MinGW build.
- Backends: WASAPI (shared with exclusive fallback) on the default render
  endpoint, and ASIO via `ASIO4ALL v2` with the Steinberg ASIO SDK 2.3
  supplied outside the repository.
- Stream format: 48 kHz, stereo, retained and verified.
- Duration: 200 seconds per buffer. This is screening evidence. It is
  **not** the two-hour Week 2 exit soak.

Two physical cores matter to the reading below: this machine has less
scheduling headroom than a typical audio workstation, and that is a
property of the declared reference machine, not a defect being excused.

## Finding 1 — WASAPI has exactly one legal period on this endpoint, and it is 512 frames

Endpoint capability, from `--list-devices` (an inventory query, not a
measured run):

```text
device_inventory count=6
device id="{0.0.0.00000000}.{618d2f42-b072-4aed-9b7f-a53b646cfeab}" name="Speaker (Realtek(R) Audio)" inputs=0 outputs=2 buffer_frames=512..512 rates=48000
device id="{0.0.0.00000000}.{2a56d4f2-2710-42d8-9fe6-5cb63ef55893}" name="CABLE Input (VB-Audio Virtual Cable)" inputs=0 outputs=2 buffer_frames=128..480 rates=48000
device id="{0.0.0.00000000}.{4095b5ff-d637-4831-a9a6-61ea5c2f682b}" name="CABLE In 16ch (VB-Audio Virtual Cable)" inputs=0 outputs=2 buffer_frames=128..480 rates=48000
device id="{0.0.1.00000000}.{4b2903fe-4aa4-4199-a1fc-3316e81d4737}" name="Stereo Mix (Realtek(R) Audio)" inputs=2 outputs=0 buffer_frames=480..480 rates=48000
device id="{0.0.1.00000000}.{688982a0-d75e-49b7-87c4-ee0716d79486}" name="Microphone Array (Realtek(R) Audio)" inputs=2 outputs=0 buffer_frames=480..480 rates=48000
device id="{0.0.1.00000000}.{d670f117-1607-4d83-b762-5d558bcfdb38}" name="CABLE Output (VB-Audio Virtual Cable)" inputs=2 outputs=0 buffer_frames=480..480 rates=48000
```

For the default render endpoint,
`IAudioClient3::GetSharedModeEnginePeriod` returns minimum = maximum =
fundamental = 512 frames. Shared mode therefore admits one period on this
endpoint and it is 10.67 ms. The probe falls back to exclusive mode,
which rejects both small sizes outright:

```text
buffer=64 status=unsupported_device_period backend=WASAPI_exclusive stream_buffer=0 period_min=512 period_max=512 period_fundamental=512
buffer=128 status=unsupported_device_period backend=WASAPI_exclusive stream_buffer=0 period_min=512 period_max=512 period_fundamental=512
```

This is a property of the endpoint and its driver. It is not a probe
defect and not something Iramix can configure around: both WASAPI modes
were attempted and both refused.

The only endpoints on this machine reporting a smaller minimum are the
two VB-Audio virtual cables (128..480 frames). They are software loopback
devices with no hardware clock, so they are not admissible as callback
performance evidence and were not measured.

## Finding 2 — ASIO4ALL accepts 64 frames but does not sustain its cadence

`ASIO4ALL v2` advertises 64–2048 frames at 8-frame granularity, so all
three requested sizes open. What differs is whether the callbacks arrive.

| Buffer | Expected callbacks | Observed | Coverage | Late wakeups above 1.5× period | Screening result |
|---:|---:|---:|---:|---:|---|
| 64 | 150,000 | 125,172 | 83.448% | 12,676 | Failed cadence screening |
| 128 | 75,000 | 75,000 | 100.000% | 179 | Scheduling warnings retained |
| 256 | 37,500 | 37,500 | 100.000% | 2 | Clean short screening |

Expected counts are `duration × 48,000 / buffer-size`. The 64-frame
stream delivered 24,828 fewer callbacks than nominal, a deficit of
16.552%.

Against the 2026-07-27 screening (same driver, same machine, same MSVC
Release build type, minimal callback workload) the 64-frame deficit grew
from 12.584% to 16.552%, and the 128-frame stream went from a clean
nominal count with 88 late wakeups to a clean nominal count with 179.
The production graph costs more per callback than the stand-in workload
did, so more scheduling headroom is consumed — but see Finding 4 before
reading that as the cause.

## Finding 3 — the deficit is steady-state, not a startup artifact

This was the open question from 2026-07-27, which retained only an
aggregate late-wakeup count. The ten equal-duration buckets (each a
20-second window of the 200-second run) now resolve it:

| Buffer | Bucket 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 64 | 1,444 | 705 | 1,156 | 942 | 1,632 | 1,383 | 1,471 | 1,169 | 1,398 | 1,376 |
| 128 | 21 | 10 | 18 | 11 | 9 | 15 | 27 | 19 | 26 | 23 |
| 256 | 0 | 0 | 0 | 1 | 0 | 1 | 0 | 0 | 0 | 0 |

Every 20-second window of the 64-frame run carries between 705 and 1,632
late wakeups. Nothing clusters at the start, at the end, or anywhere
else. The 64-frame cadence failure is a continuous property of the
configuration, so it cannot be dismissed as warm-up, JIT, page faults, or
a one-off scheduling event, and a longer run would not average it away.

## Finding 4 — callback execution time is not the constraint

| Buffer | Target p99 | p50 | p95 | p99 | Maximum | Target misses | Hard deadline misses |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 64 | ≤0.93 ms | 0.004600 ms | 0.012400 ms | 0.012900 ms | 0.360200 ms | 0 | 0 |
| 128 | ≤1.87 ms | 0.008800 ms | 0.016600 ms | 0.017300 ms | 0.783100 ms | 0 | 0 |
| 256 | ≤3.73 ms | 0.013800 ms | 0.024700 ms | 0.026700 ms | 0.566000 ms | 0 | 0 |

At 64 frames the production graph consumes 0.0129 ms at p99 against a
0.93 ms budget — 1.4% of the period. Zero target misses and zero hard
deadline misses at every size. The engine finishes its work with room to
spare in the very configuration that fails; what fails is the delivery of
callbacks to it.

WASAPI exclusive at 256 frames, same machine, same build, for comparison
of the engine's own cost across backends:

| Backend | Buffer | p50 | p95 | p99 | Maximum | Late wakeups |
|---|---:|---:|---:|---:|---:|---:|
| WASAPI exclusive | 256 | 0.011900 ms | 0.016600 ms | 0.022900 ms | 0.299200 ms | 2 |
| ASIO4ALL v2 | 256 | 0.013800 ms | 0.024700 ms | 0.026700 ms | 0.566000 ms | 2 |

## Real-time audit proof

Both executables run a positive audit self-test first: they deliberately
perform one allocation, one deallocation, and one `TrackedMutex::lock()`
inside a callback scope and refuse to run if any hook fails to record the
violation.

Every measured configuration on both backends reported
`callback_allocations=0`, `callback_deallocations=0`, and
`callback_blocking_locks=0`, with `callback_subnormal_samples_flushed=0`.
The ASIO figures now cover the production graph rather than a stand-in
workload, which is the first time the immutable render plan has been
audited under an ASIO driver.

This proves zero heap operations through the overridden global operators
and zero blocking calls through Iramix's tracked mutex inside the audited
callback scope. It makes no claim about allocations or locks inside the
driver or operating system outside that scope.

## Interpretation, stated as a hypothesis

`ASIO4ALL v2` is not a hardware driver. It is a wrapper that drives the
same Windows audio endpoints measured in Finding 1, whose engine period
is fixed at 512 frames. A 64-frame ASIO buffer is therefore a software
subdivision of a hardware period eight times larger, and a subdivision
that the underlying endpoint never asked for is exactly the shape of a
steady, evenly distributed callback deficit.

This is consistent with all three findings and it is **not proven by
them**. The probe cannot see inside the driver, and no native hardware
ASIO interface was available to test the alternative. It is recorded here
as the hypothesis a native interface would confirm or refute, not as a
conclusion.

## Raw process output

ASIO, retained locally at
`build/audio-probe/asio-graph-screening-2026-07-29.stdout.txt`:

```text
audio_probe backend=ASIO driver="ASIO4ALL v2" sample_rate=48000 seconds_per_buffer=200 driver_min=64 driver_max=2048 driver_preferred=512 driver_granularity=8 callback_workload=immutable_graph_production_nodes
buffer=64 status=measured backend=ASIO callbacks=125172 expected_callbacks=150000 callback_coverage=0.834480 p50_ms=0.004600 p95_ms=0.012400 p99_ms=0.012900 max_ms=0.360200 target_misses=0 hard_deadline_misses=0 late_wakeups=12676 late_wakeup_buckets_10=1444,705,1156,942,1632,1383,1471,1169,1398,1376 reset_requests=0 resync_requests=0 sample_rate_changes=0 callback_allocations=0 callback_deallocations=0 callback_blocking_locks=0 callback_denormal_mode_entries=125272 callback_subnormal_samples_flushed=0 graph_blocks=125272 graph_generation=1 telemetry_dropped=0
buffer=128 status=measured backend=ASIO callbacks=75000 expected_callbacks=75000 callback_coverage=1.000000 p50_ms=0.008800 p95_ms=0.016600 p99_ms=0.017300 max_ms=0.783100 target_misses=0 hard_deadline_misses=0 late_wakeups=179 late_wakeup_buckets_10=21,10,18,11,9,15,27,19,26,23 reset_requests=0 resync_requests=0 sample_rate_changes=0 callback_allocations=0 callback_deallocations=0 callback_blocking_locks=0 callback_denormal_mode_entries=75100 callback_subnormal_samples_flushed=0 graph_blocks=75100 graph_generation=1 telemetry_dropped=0
buffer=256 status=measured backend=ASIO callbacks=37500 expected_callbacks=37500 callback_coverage=1.000000 p50_ms=0.013800 p95_ms=0.024700 p99_ms=0.026700 max_ms=0.566000 target_misses=0 hard_deadline_misses=0 late_wakeups=2 late_wakeup_buckets_10=0,0,0,1,0,1,0,0,0,0 reset_requests=0 resync_requests=0 sample_rate_changes=0 callback_allocations=0 callback_deallocations=0 callback_blocking_locks=0 callback_denormal_mode_entries=37600 callback_subnormal_samples_flushed=0 graph_blocks=37600 graph_generation=1 telemetry_dropped=0
```

WASAPI, retained locally at
`build/audio-probe/wasapi-graph-screening-2026-07-29.stdout.txt`:

```text
audio_probe backend=WASAPI_shared_with_exclusive_fallback sample_rate=48000 seconds_per_buffer=200 callback_workload=immutable_graph_production_nodes
buffer=64 status=unsupported_device_period backend=WASAPI_exclusive stream_buffer=0 period_min=512 period_max=512 period_fundamental=512
buffer=128 status=unsupported_device_period backend=WASAPI_exclusive stream_buffer=0 period_min=512 period_max=512 period_fundamental=512
buffer=256 status=measured backend=WASAPI_exclusive stream_buffer=256 period_min=512 period_max=512 period_fundamental=512 callbacks=37500 p50_ms=0.011900 p95_ms=0.016600 p99_ms=0.022900 max_ms=0.299200 target_misses=0 hard_deadline_misses=0 late_wakeups=2 wait_timeouts=0 callback_allocations=0 callback_deallocations=0 callback_blocking_locks=0 callback_denormal_mode_entries=37600 callback_subnormal_samples_flushed=0 graph_blocks=37600 graph_generation=2 graph_observed_swaps=2 reclaimed_plans=1 hot_swap=completed automation=queued parameter_pending=0 parameter_rejected=0 parameter_late=0 parameter_overflow=0 reset_command=queued reset_completion=applied command_pending=0 command_rejected=0 completion_lost=0 telemetry_received=37600 telemetry_dropped=0 graph_control_failed=false mmcss=enabled
```

## Evidence boundary

This result does **not** prove:

- that 64 frames is unachievable on Windows. It is unachievable on *this
  machine's* endpoints and through *this* wrapper driver. A native
  hardware interface with its own clock and its own ASIO driver was not
  available and would very likely report a different engine period. The
  claim is about the declared reference machine and nothing wider.
- that the ASIO4ALL-wraps-WASAPI explanation is correct. It is a
  hypothesis consistent with Findings 1–4 and untested against a native
  driver, as stated above.
- anything about macOS or Linux. The Core Audio and JACK probes exist and
  have never been run on target hardware (R-13, accepted for Phase 0).
- that there are no audible dropouts. There is no acoustic loopback and
  no driver-level xrun counter in this evidence. `resync_requests=0` and
  `sample_rate_changes=0` are what the driver chose to report, not an
  independent measurement.
- anything about sustained behavior. 200 seconds per buffer is screening;
  the two-hour Week 2 exit soak has still never been run on any operating
  system. Finding 3 shows the 64-frame deficit is steady, which argues a
  longer run would confirm rather than dissolve it, but that is inference
  and not the soak.
- that the ASIO path exercises graph control. `graph_generation=1` on
  every ASIO configuration: unlike the WASAPI probe, the ASIO probe does
  not hot-swap plans, enqueue automation, or issue realtime commands
  while running. Plan swapping under ASIO is unmeasured.
- that the callback-duration percentiles have fine resolution at the
  tail. Percentiles are nearest-rank over the observed callbacks.
- anything about a MinGW build. Both binaries here are MSVC Release.

## Remaining P0-008 evidence

- a native hardware ASIO interface on which 64 frames can be attempted
  without a wrapper driver — this is a hardware acquisition, and until it
  exists the 64-frame budget in `PERFORMANCE_BUDGETS.md` is unvalidated
  rather than met or failed;
- driver-level xrun evidence or acoustic loopback verification;
- Core Audio and JACK runs on target hardware (blocked by R-13);
- the two-hour Week 2 exit soak on every operating system;
- graph hot-swap and automation exercised under ASIO as well as WASAPI;
- the Steinberg ASIO developer agreement before ASIO enters a
  distributable public build; signing does not block technical spikes.
