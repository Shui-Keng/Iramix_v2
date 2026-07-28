# Windows ASIO Audio Callback Screening — 2026-07-27

Status: Windows baseline evidence; P0-008 remains open.

## Reference environment

- OS: Windows 11 Home Single Language 64-bit, build 10.0.26200.
- CPU: AMD Athlon Silver 3050U with Radeon Graphics.
- Memory: 21.9 GiB.
- Build: MSVC Release.
- Backend and driver: ASIO, `ASIO4ALL v2`.
- External SDK: Steinberg ASIO SDK 2.3, supplied outside the repository.
- Stream format: requested and retained at 48 kHz, stereo output.
- Timed workload: preallocated stereo gain over silence plus clearing the two
  native output buffers.
- Duration: 200 seconds per buffer, 600 seconds total.

This is a short screening soak, not the two-hour Week 2 exit soak. It tests a
minimal callback workload rather than a production DAW graph.

## Callback execution results

| Buffer | Target p99 | p50 | p95 | p99 | Maximum | Target misses | Hard deadline misses |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 64 | ≤0.93 ms | 0.000400 ms | 0.001800 ms | 0.002100 ms | 0.197700 ms | 0 | 0 |
| 128 | ≤1.87 ms | 0.000500 ms | 0.002100 ms | 0.002400 ms | 0.284600 ms | 0 | 0 |
| 256 | ≤3.73 ms | 0.001500 ms | 0.002500 ms | 0.002800 ms | 0.166400 ms | 0 | 0 |

All three callback-duration p99 values are below their respective engine
targets. This result is narrowly about execution time; it does not override
the scheduling evidence below.

## Callback cadence and dropout screening

| Buffer | Expected callbacks | Observed callbacks | Coverage | Late wakeups above 1.5× period | Screening result |
|---:|---:|---:|---:|---:|---|
| 64 | 150,000 | 131,124 | 87.416% | 211 | Failed cadence screening |
| 128 | 75,000 | 75,000 | 100.000% | 88 | Scheduling warnings retained |
| 256 | 37,500 | 37,500 | 100.000% | 0 | Clean short screening |

Expected callback counts are derived from
`duration × 48,000 / buffer-size`. The 64-frame stream delivered 18,876 fewer
callbacks than the nominal count, an observed deficit of 12.584%. The probe
cannot distinguish driver aggregation, scheduling loss, or another
ASIO4ALL/hardware interaction, so the 64-frame configuration is explicitly
not accepted even though its callback execution time was short.

The 128-frame stream reached its nominal callback count but retained 88 late
wakeup signals. The ASIO driver reported zero resynchronization requests and
zero sample-rate-change notifications for all configurations. There was no
acoustic loopback or hardware xrun counter, so these figures must not be
reported as proof of zero audible dropouts.

Distribution note: the completed soak retained only the aggregate count, not
per-event timestamps or time buckets, so whether the 88 late wakeups clustered
in one part of the run or were randomly distributed cannot be recovered.
The probe was subsequently updated to emit ten fixed, preallocated time
buckets for each configuration; the next screening and two-hour soak will
retain that distribution without allocating inside the callback.

## Real-time audit proof

The executable first runs a positive audit self-test. It deliberately performs
one allocation, one deallocation, and one `TrackedMutex::lock()` inside a
callback scope and refuses to run if any hook fails to record the violation.

The actual ASIO callback scope includes the stereo gain workload and clearing
both driver output buffers. Global `operator new`/`delete` variants and the
project blocking-lock wrapper recorded:

| Buffer | Allocations | Deallocations | Tracked blocking locks |
|---:|---:|---:|---:|
| 64 | 0 | 0 | 0 |
| 128 | 0 | 0 | 0 |
| 256 | 0 | 0 | 0 |

This proves zero heap operations through the overridden global operators and
zero blocking calls through Iramix's tracked mutex inside the audited callback
scope. It does not make a claim about allocations or locks inside the driver
or operating system outside that scope.

## Raw process output

```text
audio_probe backend=ASIO driver="ASIO4ALL v2" sample_rate=48000 seconds_per_buffer=200 driver_min=64 driver_max=2048 driver_preferred=512 driver_granularity=8
buffer=64 status=measured backend=ASIO callbacks=131124 p50_ms=0.000400 p95_ms=0.001800 p99_ms=0.002100 max_ms=0.197700 target_misses=0 hard_deadline_misses=0 late_wakeups=211 resync_requests=0 sample_rate_changes=0 callback_allocations=0 callback_deallocations=0 callback_blocking_locks=0
buffer=128 status=measured backend=ASIO callbacks=75000 p50_ms=0.000500 p95_ms=0.002100 p99_ms=0.002400 max_ms=0.284600 target_misses=0 hard_deadline_misses=0 late_wakeups=88 resync_requests=0 sample_rate_changes=0 callback_allocations=0 callback_deallocations=0 callback_blocking_locks=0
buffer=256 status=measured backend=ASIO callbacks=37500 p50_ms=0.001500 p95_ms=0.002500 p99_ms=0.002800 max_ms=0.166400 target_misses=0 hard_deadline_misses=0 late_wakeups=0 resync_requests=0 sample_rate_changes=0 callback_allocations=0 callback_deallocations=0 callback_blocking_locks=0
```

The raw stdout is also retained locally at
`build/audio-probe/asio-soak-2026-07-27.stdout.txt`.

## Driver screening notes

- `ASIO4ALL v2` exposed 64–2048 frames with an 8-frame granularity and was
  selected because it opened all three requested sizes.
- `FL Studio ASIO` exposed a 256-frame minimum, so it could not cover 64 or
  128 frames.
- `FlexASIO` did not initialize because its existing user configuration named
  an unavailable endpoint. The probe did not modify global user configuration.

## Remaining exit evidence

- investigate or reproduce the 64-frame cadence deficit on a native hardware
  ASIO driver and a declared reference audio interface;
- retain driver-level xrun evidence or add acoustic loopback verification;
- run equivalent Core Audio and Linux backend probes;
- complete the full two-hour Week 2 soak;
- run the production-like graph workload once the immutable graph spike exists;
- complete the selected proprietary ASIO developer agreement before ASIO enters
  a distributable public build; signing does not block further technical
  spikes.
