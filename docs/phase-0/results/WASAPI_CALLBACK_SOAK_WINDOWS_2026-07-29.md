# WASAPI Two-Hour Callback Soak — 2026-07-29

Status: the second full-length soak on the reference machine, and the
first on WASAPI. It exists to answer one question the ASIO soak raised:
is the small callback-delivery deficit a property of the wrapper driver
or of the endpoint underneath it?

Answer: **both backends lose callbacks, and WASAPI loses fewer.** The
deficit is not created solely by `ASIO4ALL v2`, and it is not solely the
endpoint's either. See the comparison section for how little that
sentence is allowed to claim.

## Reference environment

- OS: Windows 11 Home Single Language 64-bit, build 26200.
- CPU: AMD Athlon Silver 3050U with Radeon Graphics, 2 cores / 2 threads.
- Memory: 21.9 GiB.
- Power: on AC, "High performance" power scheme.
- Build: MSVC Release (`windows-msvc` preset, `Release` configuration),
  source revision `508d927`.
- Backend: WASAPI, shared mode attempted first and rejected, so the
  measured stream is **exclusive** mode.
- Stream format: 48 kHz, stereo, retained and verified.
- Callback workload: the production immutable graph, reported as
  `callback_workload=immutable_graph_production_nodes`.
- Duration: 2,400 seconds at 256 frames.
- Machine state: no build, test, or other workload was started by this
  session during the run. One idle Gradle daemon from another worktree
  remained resident and was measured at 0.016 CPU-seconds over 8
  seconds — roughly 0.2% of one core — before the soak began. It was
  left running rather than killed, and is recorded here rather than
  claimed away: the machine was quiet, not sterile.

The process exits with code 4 because the 64- and 128-frame
configurations are rejected. That is the documented behavior for this
endpoint (R-15), not a soak failure; the 256-frame configuration ran to
completion.

## Result

| Buffer | Expected callbacks | Observed | Coverage | Target misses | Hard deadline misses |
|---:|---:|---:|---:|---:|---:|
| 256 | 450,000 | 449,988 | 99.99733% | 0 | 0 |

| Buffer | p99 target | p50 | p95 | p99 | Maximum | Hard deadline |
|---:|---:|---:|---:|---:|---:|---:|
| 256 | ≤3.73 ms | 0.011200 ms | 0.017900 ms | 0.021100 ms | 0.573900 ms | 5.33 ms |

p99 consumes 0.57% of the budget. The worst single callback in 449,988
reached 0.5739 ms — 10.8% of the hard deadline. One late wakeup in the
entire 40 minutes.

## Comparison with the ASIO soak, and its limits

Same machine, same MSVC Release build type, same production graph
workload, same 2,400-second duration, same 256-frame buffer. Only the
backend differs, which is what makes this pair worth putting side by
side at all.

| | WASAPI exclusive | ASIO4ALL v2 |
|---|---:|---:|
| Observed callbacks | 449,988 | 449,956 |
| Missing against nominal | 12 | 44 |
| Coverage | 99.99733% | 99.9902% |
| Late wakeups | 1 | 22 |
| p50 | 0.011200 ms | 0.013800 ms |
| p99 | 0.021100 ms | 0.026700 ms |
| Maximum | 0.573900 ms | 1.690600 ms |

Both lose callbacks. WASAPI loses fewer, and its tail is roughly three
times tighter at the maximum.

**What this does not establish.** These are one run each. Twelve and
forty-four are small counts drawn from 450,000, and nothing here
estimates their run-to-run variance — a second pair could plausibly
land closer together or further apart. The ratio between them is not a
measurement, and must not be quoted as "ASIO4ALL is 3.7× worse". What
the pair supports is the weaker and more useful statement: **the
deficit does not vanish when the wrapper is removed**, so attributing
it entirely to `ASIO4ALL v2` would have been wrong.

It also cannot separate backend from mode. The WASAPI stream is
exclusive mode with its own period, while ASIO4ALL wraps the same
endpoint through the shared engine. Two things changed between the two
columns, not one.

## Graph control was exercised, at soak length

Unlike the ASIO probe, the WASAPI probe drives plan hot-swap,
parameter automation, and a realtime command during the run. Over the
full 2,400 seconds:

```text
graph_generation=2 graph_observed_swaps=2 reclaimed_plans=1
hot_swap=completed automation=queued
parameter_pending=0 parameter_rejected=0 parameter_late=0 parameter_overflow=0
reset_command=queued reset_completion=applied
command_pending=0 command_rejected=0 completion_lost=0
telemetry_received=450088 telemetry_dropped=0
```

A plan swap completed while audio ran, the retired plan was reclaimed
off the audio thread, the queued automation was applied with no late or
rejected events, and the realtime reset command was acknowledged. No
command completion was lost and no telemetry was dropped across 450,088
blocks.

This partially closes an item the ASIO soak listed as remaining, but
only partially: the swap happens **once**, early in the run
(`graph_observed_swaps=2` covers the initial publication and the one
replacement). Repeated topology churn sustained across 40 minutes is
still unmeasured — the sanitizer edit-storm in P0-011 covers 5,001
publications but not against a live device callback.

## Real-time audit proof

| Allocations | Deallocations | Tracked blocking locks | Subnormals flushed | Wait timeouts |
|---:|---:|---:|---:|---:|
| 0 | 0 | 0 | 0 | 0 |

Zero heap operations through the overridden global operators and zero
blocking calls through Iramix's tracked mutex, across 450,088 blocks.
Combined with the ASIO soak, the no-allocation property now holds across
**1,799,407 measured callbacks and 80 minutes on two independent
backends**.

`mmcss=enabled`: the callback thread was registered with the Multimedia
Class Scheduler Service for the whole run.

## Raw process output

Retained locally at
`build/audio-probe/wasapi-soak-2h-2026-07-29.stdout.txt`:

```text
audio_probe backend=WASAPI_shared_with_exclusive_fallback sample_rate=48000 seconds_per_buffer=2400 callback_workload=immutable_graph_production_nodes
buffer=64 status=unsupported_device_period backend=WASAPI_exclusive stream_buffer=0 period_min=512 period_max=512 period_fundamental=512
buffer=128 status=unsupported_device_period backend=WASAPI_exclusive stream_buffer=0 period_min=512 period_max=512 period_fundamental=512
buffer=256 status=measured backend=WASAPI_exclusive stream_buffer=256 period_min=512 period_max=512 period_fundamental=512 callbacks=449988 p50_ms=0.011200 p95_ms=0.017900 p99_ms=0.021100 max_ms=0.573900 target_misses=0 hard_deadline_misses=0 late_wakeups=1 wait_timeouts=0 callback_allocations=0 callback_deallocations=0 callback_blocking_locks=0 callback_denormal_mode_entries=450088 callback_subnormal_samples_flushed=0 graph_blocks=450088 graph_generation=2 graph_observed_swaps=2 reclaimed_plans=1 hot_swap=completed automation=queued parameter_pending=0 parameter_rejected=0 parameter_late=0 parameter_overflow=0 reset_command=queued reset_completion=applied command_pending=0 command_rejected=0 completion_lost=0 telemetry_received=450088 telemetry_dropped=0 graph_control_failed=false mmcss=enabled
```

## Evidence boundary

This result does **not** prove:

- that the WASAPI/ASIO difference is real at the size stated. One run
  each, counts of 12 and 44 out of 450,000, no variance estimate. Only
  the direction and the "deficit survives without the wrapper" finding
  are supported.
- anything about 64 or 128 frames on WASAPI. Both remain unopenable on
  this endpoint (R-15); this soak measures only 256.
- anything about shared mode. Shared mode was attempted and rejected —
  the endpoint's one legal engine period is 512 frames — so every figure
  here is exclusive mode.
- anything about macOS or Linux. Neither Core Audio nor JACK has been
  run on target hardware at any duration (R-13, accepted).
- that there are no audible dropouts. No acoustic loopback and no
  driver-level xrun counter. The 12 undelivered callbacks were not
  listened to.
- that sustained topology churn is safe under a live callback. One plan
  swap was measured, not a stream of them.
- that behavior holds on a busy machine. The soak ran on a deliberately
  quiet laptop with one dormant Gradle daemon resident. Real use shares
  the machine with a UI process.
- that the 0.5739 ms maximum is understood. It is recorded, not
  explained; the probe measures duration, not cause.
- anything about a MinGW build. This binary is MSVC Release.

## Remaining P0-008 evidence

- Core Audio and JACK soaks on target hardware (blocked by R-13);
- repeated plan swaps sustained across a soak, rather than one;
- driver-level xrun evidence or acoustic loopback verification;
- run-to-run variance for the delivery deficit, if the difference
  between backends is ever to be claimed as a magnitude;
- the Steinberg ASIO developer agreement before ASIO enters a
  distributable public build; signing does not block technical spikes.
