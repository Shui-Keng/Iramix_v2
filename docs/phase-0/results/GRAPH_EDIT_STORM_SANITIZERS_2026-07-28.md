# Graph Edit-Storm and Sanitizer Evidence — 2026-07-28

Status: P0-011 graph replacement, reclamation, and sanitizer slice verified.
This result does not close P0-008 or the full Week 5 gate.

## Scope

The control thread repeatedly compiles and publishes five deterministic graph
variants while a dedicated audio thread continuously renders 64-frame blocks:

1. direct source-to-output routing;
2. one processing stage;
3. two-source fan-in;
4. parallel paths with a 17-sample latency difference and PDC;
5. two serial processing stages.

There are 5,001 total publications: the initial plan plus 5,000 live graph
edits. The test requires the final generation to be acknowledged, every
superseded plan to be reclaimed exactly once, no retired plans to remain, and
the final graph to render the expected sample value.

The callback audit separately requires zero allocation, deallocation, and
blocking-lock calls. Telemetry is intentionally left unconsumed so the test
also proves that saturation is bounded and counted instead of blocking audio.

## Windows local result

Environment: Windows development build using the `dev` preset.

```text
Graph edit storm: variants=5, publications=5001, blocks=2383586,
observed_swaps=4194, reclaimed=5000, retired=0, allocations=0,
deallocations=0, blocking_locks=0, use_after_free=0,
telemetry_dropped=2381538
```

All four CTest tests passed. Block and observed-swap counts are scheduler
dependent; publication, reclamation, lifetime, audit, and final-output
assertions are deterministic.

## Linux sanitizer results

Source commit: `e8bdbdb80d9d908fa4679bc777bb1c5e6b276061`  
GitHub Actions:
[`30334889749`](https://github.com/Shui-Keng/Iramix_v2/actions/runs/30334889749)

### AddressSanitizer and UndefinedBehaviorSanitizer

```text
Graph edit storm: variants=5, publications=5001, blocks=526472,
observed_swaps=4986, reclaimed=5000, retired=0, allocations=0,
deallocations=0, blocking_locks=0, use_after_free=0,
telemetry_dropped=524424
```

All four tests passed in 1.74 seconds. The job used
`detect_leaks=1:halt_on_error=1` for ASan and
`halt_on_error=1:print_stacktrace=1` for UBSan. No sanitizer error or runtime
undefined-behavior report was emitted.

### ThreadSanitizer

```text
Graph edit storm: variants=5, publications=5001, blocks=136424,
observed_swaps=4999, reclaimed=5000, retired=0, allocations=0,
deallocations=0, blocking_locks=0, use_after_free=0,
telemetry_dropped=134376
```

All four tests passed in 1.47 seconds with
`halt_on_error=1:second_deadlock_stack=1`. No ThreadSanitizer warning was
emitted.

## Interpretation and boundary

This closes the initial edit-storm and ASan/UBSan/TSan evidence item for the
immutable render-plan publication path. It is suitable for hosted CI because
the test is deterministic engine logic and does not open an audio device.

It is not evidence that Core Audio, JACK, ALSA, or any physical interface meets
callback timing or dropout targets. Those measurements require real target
machines and devices. P0-008 therefore remains open, including all mandatory
two-hour hardware soaks.

Parameter ramps, modulation-rate streams, and denormal handling were completed
in the subsequent
[`PARAMETER_RAMP_DENORMAL_2026-07-28.md`](PARAMETER_RAMP_DENORMAL_2026-07-28.md)
slice. Production integration for non-WASAPI backends and final hardware-soak
evidence remain pending.
