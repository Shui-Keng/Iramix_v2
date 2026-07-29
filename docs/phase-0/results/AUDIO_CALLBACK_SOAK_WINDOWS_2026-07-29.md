# Windows Two-Hour Callback Soak — 2026-07-29

Status: the Week 2 exit soak, run for the first time on any operating
system. The engine caused no dropouts in 1,349,419 callbacks. P0-008
remains open for macOS and Linux (R-13) and for 64 frames (R-15).

## Scope, stated before the numbers

The runbook's unit is **2,400 seconds per buffer configuration**, and both
measured configurations received their full 2,400 seconds. The aggregate
is 4,800 seconds rather than the runbook's 7,200 because the 64-frame
configuration was dropped: no device available to this project delivers
that period, which is measured in
[`AUDIO_CALLBACK_64_FRAME_FEASIBILITY_2026-07-29.md`](AUDIO_CALLBACK_64_FRAME_FEASIBILITY_2026-07-29.md)
and accepted as risk R-15. Soaking it would have spent 40 minutes
re-measuring a configuration already known to be unreachable.

So this document does not claim "the two-hour soak passed" as a whole. It
claims that the two configurations this hardware can run each completed a
full-length soak.

## Reference environment

- OS: Windows 11 Home Single Language 64-bit, build 26200.
- CPU: AMD Athlon Silver 3050U with Radeon Graphics, 2 cores / 2 threads.
- Memory: 21.9 GiB.
- Power: on AC, "High performance" power scheme.
- Build: MSVC Release (`windows-msvc` preset, `Release` configuration).
- Backend and driver: ASIO via `ASIO4ALL v2`, Steinberg ASIO SDK 2.3
  supplied outside the repository.
- Stream format: 48 kHz, stereo, retained and verified.
- Callback workload: the production immutable graph
  (`DeviceInput → Track → Gain → Mixer → Output` through
  `RenderPlanExecutor`), reported as
  `callback_workload=immutable_graph_production_nodes`.
- Duration: 2,400 seconds each at 128 and 256 frames; 4,800 seconds total.
- Machine state: idle for the whole run. No build, test, or other
  workload was started by this session while the soak was in progress.
  An earlier attempt was stopped and its partial log discarded rather
  than kept, because the machine was needed for other work.

## Result

| Buffer | Expected callbacks | Observed | Coverage | Target misses | Hard deadline misses |
|---:|---:|---:|---:|---:|---:|
| 128 | 900,000 | 899,463 | 99.9403% | 0 | 0 |
| 256 | 450,000 | 449,956 | 99.9902% | 0 | 0 |

| Buffer | p99 target | p50 | p95 | p99 | Maximum | Hard deadline |
|---:|---:|---:|---:|---:|---:|---:|
| 128 | ≤1.87 ms | 0.007800 ms | 0.011000 ms | 0.020300 ms | 1.827200 ms | 2.67 ms |
| 256 | ≤3.73 ms | 0.012100 ms | 0.017500 ms | 0.031700 ms | 1.690600 ms | 5.33 ms |

At p99 the engine consumes 1.09% of its budget at 128 frames and 0.85% at
256. Zero target misses and zero hard deadline misses across 1,349,419
measured callbacks.

## The single worst callback deserves naming

The maximum at 128 frames is **1.8272 ms against a 2.67 ms hard
deadline** — 68.4% of the period consumed by one callback, and roughly 90
times the p99. It did not miss, and one outlier in 899,463 callbacks is
not a trend. But it is close enough to the deadline that it should not be
buried under a p99 figure of 0.0203 ms, and a machine with less headroom,
or one more scheduling event landing on the same callback, would have
crossed it. The 256-frame maximum of 1.6906 ms sits at 31.7% of its
larger deadline and is less interesting.

Nothing in this evidence attributes that outlier. The probe records
duration, not cause.

## Callback delivery is not perfect, and that is a driver property

Coverage is 99.9403% at 128 frames — 537 callbacks short of nominal over
40 minutes — and 99.9902% at 256, 44 short. The 200-second screening
reported 100.000% at both sizes, so these deficits only become visible at
soak length.

| Buffer | Late wakeups | Share of callbacks | Ten equal windows (4 minutes each) |
|---:|---:|---:|---|
| 128 | 1,492 | 0.1659% | 134, 135, 153, 151, 147, 143, 152, 148, 183, 146 |
| 256 | 22 | 0.0049% | 2, 10, 1, 0, 2, 2, 0, 1, 2, 2 |

The 128-frame late wakeups are evenly distributed — every four-minute
window carries between 134 and 183, with no drift across 40 minutes. This
is the same steady-state shape as the 64-frame deficit in the feasibility
result, three orders of magnitude smaller: 0.06% here against 16.55%
there.

These are **not engine-caused dropouts**. The engine met every deadline it
was given; what varies is when the driver delivered the callback. The
distinction matters because the Phase 0 gate is specifically about
engine-caused dropouts, and conflating the two would either falsely
implicate the engine or falsely clear the driver.

**Update, same day:** this section originally read the deficit as the
wrapper driver's own cadence. A WASAPI soak at the same 256-frame size
([`WASAPI_CALLBACK_SOAK_WINDOWS_2026-07-29.md`](WASAPI_CALLBACK_SOAK_WINDOWS_2026-07-29.md))
also lost callbacks — 12 against ASIO4ALL's 44 — so the deficit survives
without `ASIO4ALL v2` in the path and cannot be attributed to it alone.
WASAPI lost fewer, but one run each cannot turn that into a magnitude.

## Real-time audit proof

The executable runs a positive audit self-test first: it deliberately
performs one allocation, one deallocation, and one `TrackedMutex::lock()`
inside a callback scope and refuses to run if any hook fails to record the
violation.

| Buffer | Allocations | Deallocations | Tracked blocking locks | Subnormals flushed | Telemetry dropped |
|---:|---:|---:|---:|---:|---:|
| 128 | 0 | 0 | 0 | 0 | 0 |
| 256 | 0 | 0 | 0 | 0 | 0 |

Zero heap operations through the overridden global operators and zero
blocking calls through Iramix's tracked mutex, sustained across 1,349,419
callbacks and 80 minutes. This is the first evidence that the no-allocation
property holds over soak length rather than over a screening run; a leak or
a rare allocation path would have had 899,563 blocks at 128 frames to
appear in.

`telemetry_dropped=0` at both sizes means the bounded telemetry queue was
drained faster than it filled for the whole soak, so no block's telemetry
was lost.

## Phase 0 gate status

`PERFORMANCE_BUDGETS.md` lists "zero engine-caused dropouts in a two-hour
reference soak test" as a gate. For 128 and 256 frames on Windows, that
gate is **met**: zero target misses, zero hard deadline misses, zero
allocations, zero blocking locks, over a full-length soak each.

The gate is not met for 64 frames (unreachable, R-15) and has not been
attempted on macOS or Linux (R-13).

## Raw process output

Retained locally at
`build/audio-probe/asio-soak-2h-2026-07-29.stdout.txt`:

```text
audio_probe backend=ASIO driver="ASIO4ALL v2" sample_rate=48000 seconds_per_buffer=2400 driver_min=64 driver_max=2048 driver_preferred=512 driver_granularity=8 callback_workload=immutable_graph_production_nodes requested_buffers=128,256
buffer=128 status=measured backend=ASIO callbacks=899463 expected_callbacks=900000 callback_coverage=0.999403 p50_ms=0.007800 p95_ms=0.011000 p99_ms=0.020300 max_ms=1.827200 target_misses=0 hard_deadline_misses=0 late_wakeups=1492 late_wakeup_buckets_10=134,135,153,151,147,143,152,148,183,146 reset_requests=0 resync_requests=0 sample_rate_changes=0 callback_allocations=0 callback_deallocations=0 callback_blocking_locks=0 callback_denormal_mode_entries=899563 callback_subnormal_samples_flushed=0 graph_blocks=899563 graph_generation=1 telemetry_dropped=0
buffer=256 status=measured backend=ASIO callbacks=449956 expected_callbacks=450000 callback_coverage=0.999902 p50_ms=0.012100 p95_ms=0.017500 p99_ms=0.031700 max_ms=1.690600 target_misses=0 hard_deadline_misses=0 late_wakeups=22 late_wakeup_buckets_10=2,10,1,0,2,2,0,1,2,2 reset_requests=0 resync_requests=0 sample_rate_changes=0 callback_allocations=0 callback_deallocations=0 callback_blocking_locks=0 callback_denormal_mode_entries=450056 callback_subnormal_samples_flushed=0 graph_blocks=450056 graph_generation=1 telemetry_dropped=0
```

## Evidence boundary

This result does **not** prove:

- that the soak passed at 64 frames. It was not run; no available device
  delivers the period (R-15).
- anything about WASAPI. This soak is ASIO4ALL only. *(Superseded the
  same day: WASAPI has since been soaked at 256 frames — see
  [`WASAPI_CALLBACK_SOAK_WINDOWS_2026-07-29.md`](WASAPI_CALLBACK_SOAK_WINDOWS_2026-07-29.md).
  It still opens only the 256-frame configuration on this endpoint.)*
- anything about macOS or Linux. Neither Core Audio nor JACK has been run
  on target hardware at any duration (R-13, accepted).
- that there are no audible dropouts. There is no acoustic loopback and no
  driver-level xrun counter here. `resync_requests=0` and
  `sample_rate_changes=0` are what the driver chose to report, not an
  independent observation. The 537 undelivered callbacks at 128 frames
  were not listened to.
- that graph control is safe under soak. `graph_generation=1` throughout:
  the ASIO probe does not hot-swap plans, enqueue automation, or issue
  realtime commands while running. Plan swapping during a soak is
  unmeasured on any backend — the WASAPI probe exercises it, but only at
  screening length.
- that the 1.8272 ms maximum is understood. It is recorded, not explained.
- that behavior holds on a busier machine. The soak ran on a deliberately
  idle 2-core laptop. Iramix in real use shares the machine with a UI
  process, and the GPU spike already measured this hardware's tail
  widening from 28 ms to 67 ms under concurrent load.
- that a longer run would look the same. 2,400 seconds per configuration
  is the runbook's declared soak length, not an endurance limit.
- anything about a MinGW build. This binary is MSVC Release.

## Remaining P0-008 evidence

- Core Audio and JACK soaks on target hardware (blocked by R-13);
- ~~a WASAPI soak at 256 frames, to check whether the delivery deficit is
  specific to the wrapper driver or common to the endpoint~~ — done, and
  it found the deficit on both backends
  ([`WASAPI_CALLBACK_SOAK_WINDOWS_2026-07-29.md`](WASAPI_CALLBACK_SOAK_WINDOWS_2026-07-29.md));
- graph hot-swap and automation exercised at soak length rather than
  screening length;
- driver-level xrun evidence or acoustic loopback verification;
- the Steinberg ASIO developer agreement before ASIO enters a
  distributable public build; signing does not block technical spikes.
