# Parameter Ramp, Modulation, and Denormal Evidence — 2026-07-28

Status: P0-011 parameter-ramp, modulation-stream, and denormal slice verified.
P0-008 and the full Week 5 gate remain open.

## Implemented contract

The existing fixed-capacity parameter event path now supports three event
types without adding a callback allocation:

- `value`: replace the base parameter value at an exact sample;
- `linearRamp`: move from the current base value to a target over an explicit
  positive sample count, including across process-block boundaries;
- `modulation`: replace a persistent additive modulation value at an exact
  sample, allowing a bounded per-sample modulation stream.

All event types use the same exact-capacity SPSC queue and preallocated
per-node block buffers. They retain absolute sample timestamps, deterministic
sequence ordering, explicit rejection, late-event, unknown-target, and
per-node-overflow counters.

A ramp advances on its first scheduled sample and reaches the target on the
last sample in its duration. Additive modulation remains active until another
modulation event changes it. Gain, track gain/pan, and mixer output gain use
the shared state implementation. Mute accepts value events only.

## Deterministic engine test

The test starts a six-sample gain ramp at absolute sample 100, renders two
four-sample blocks, and sends one modulation event at every sample in the
second block.

Expected output:

```text
block 100..103: 1/6, 2/6, 3/6, 4/6
block 104..107: 5/6, 1.0, 1.5, 0.75
```

Windows GCC/Ninja and MSVC both passed. Raw Windows result:

```text
Parameter ramp/modulation: ramp_samples=6, blocks=2,
modulation_events=4, denormal_mode_entries=3,
subnormal_samples_flushed=2, allocations=0, deallocations=0,
blocking_locks=0
```

A zero-duration ramp is rejected and counted. Both render blocks drain the
five valid queued events with no callback allocation, deallocation, or
blocking lock.

## Denormal handling

The outermost real-time `CallbackScope` now:

- enables FTZ and DAZ in MXCSR on x86/x64;
- enables FZ in FPCR on supported AArch64 compilers;
- leaves nested callback scopes unchanged;
- restores the caller's original floating-point control state on exit.

Instrumentation counts outer denormal-safe callback entries and explicit
subnormal flushes. Bit-pattern tests prove that positive and negative
subnormal values become positive and negative zero respectively, without
floating-point classification that could itself be affected by DAZ.

## Sanitizer and portability evidence

Source commit: `2becb50966f70a436bd28ffc84392d82d9e3fadd`  
GitHub Actions:
[`30335929746`](https://github.com/Shui-Keng/Iramix_v2/actions/runs/30335929746)

The normal build and test lane passed on Windows, macOS, and Ubuntu. The
ASan/UBSan and TSan lanes also passed all four tests.

ASan/UBSan raw result:

```text
Parameter ramp/modulation: ramp_samples=6, blocks=2,
modulation_events=4, denormal_mode_entries=3,
subnormal_samples_flushed=2, allocations=0, deallocations=0,
blocking_locks=0
```

TSan reported the same counters. No AddressSanitizer,
UndefinedBehaviorSanitizer, or ThreadSanitizer diagnostic was emitted.

## WASAPI production screening

Command:

```text
build/windows-msvc/Debug/iramix_audio_probe.exe --seconds-per-buffer 1
```

The available 256-sample exclusive stream queued one full-buffer ramp followed
by four per-sample modulation events through the production graph-control
thread. Raw result:

```text
buffer=256 status=measured backend=WASAPI_exclusive stream_buffer=256
callbacks=188 p50_ms=0.061600 p95_ms=0.077500 p99_ms=0.099700
max_ms=0.106400 target_misses=0 hard_deadline_misses=0 late_wakeups=0
wait_timeouts=0 callback_allocations=0 callback_deallocations=0
callback_blocking_locks=0 callback_denormal_mode_entries=288
callback_subnormal_samples_flushed=0 graph_blocks=288 graph_generation=2
graph_observed_swaps=2 reclaimed_plans=1 hot_swap=completed
automation=queued parameter_pending=0 parameter_rejected=0 parameter_late=0
parameter_overflow=0 reset_command=queued reset_completion=applied
command_pending=0 command_rejected=0 completion_lost=0
telemetry_received=288 telemetry_dropped=0 graph_control_failed=false
mmcss=enabled
```

The endpoint still rejected 64- and 128-sample exclusive periods, matching the
existing Windows baseline. This one-second screening is integration evidence
for the production ramp/modulation and denormal paths, not callback-soak
evidence and not proof for Core Audio, JACK, or ALSA.

## Remaining boundary

This closes the initial P0-011 ramp/modulation and denormal items. Remaining
P0-011 work is production integration evidence on non-WASAPI backends and
final hardware-soak evidence. P0-008 remains open for all missing physical
backend measurements and the mandatory two-hour soaks.
