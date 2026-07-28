# Windows Audio Callback Screening — 2026-07-27

Status: partial Phase 0 evidence; P0-008 remains open.

This document records the WASAPI screening. A later
[`ASIO follow-up`](AUDIO_CALLBACK_ASIO_WINDOWS_2026-07-27.md) opened all three
requested buffer sizes, but its 64-frame cadence failed screening.

## Reference environment

- OS: Windows 11 Home Single Language 64-bit, build 10.0.26200.
- CPU: AMD Athlon Silver 3050U with Radeon Graphics.
- Memory: 21.9 GiB.
- Build: MSVC Debug.
- Backend: WASAPI shared mode with exclusive-mode fallback.
- Stream format: 48 kHz, stereo.
- Timed workload: preallocated stereo gain over a silent input block.
- MMCSS: `Pro Audio`, critical priority.

The WASAPI buffer acquisition and release calls are outside the timed engine
callback scope. The measured callback contains only the preallocated engine
workload.

## Requested configurations

| Requested buffer | Deadline | Engine p99 target | Result |
|---:|---:|---:|---|
| 64 | 1.33 ms | ≤0.93 ms | Stream rejected: `AUDCLNT_E_INVALID_DEVICE_PERIOD` |
| 128 | 2.67 ms | ≤1.87 ms | Stream rejected: `AUDCLNT_E_INVALID_DEVICE_PERIOD` |
| 256 | 5.33 ms | ≤3.73 ms | Measured for 600 seconds |

The default endpoint exposed only a 512-frame shared-mode period. Exclusive
mode accepted exactly 256 frames but rejected 64 and 128 frames. No percentile
is reported for configurations that did not open.

## Raw measured result

```text
buffer=256 status=measured backend=WASAPI_exclusive stream_buffer=256 period_min=512 period_max=512 period_fundamental=512 callbacks=112498 p50_ms=0.002900 p95_ms=0.005100 p99_ms=0.005800 max_ms=0.301700 target_misses=0 hard_deadline_misses=0 late_wakeups=1 wait_timeouts=0 callback_allocations=0 callback_deallocations=0 callback_blocking_locks=0 mmcss=enabled
```

For the supported 256-frame configuration, p99 was below its 3.73 ms target.
This does not imply that P0-008 passed: two requested configurations could not
be opened and macOS/Linux have not run.

## Real-time audit proof

The probe executable overrides global `operator new`, `operator new[]`, and
their delete, sized-delete, nothrow, and aligned variants. A thread-local
callback scope increments atomic violation counters if a heap operation occurs.

Project blocking locks use `TrackedMutex`; its blocking `lock()` operation
increments a violation counter inside the same callback scope. Before opening
audio, a positive self-test deliberately performs one allocation, one
deallocation, and one blocking lock and asserts that all three hooks fire. The
counters are then reset before measurement.

Across 112,498 measured callbacks:

- callback allocations: 0;
- callback deallocations: 0;
- callback tracked blocking locks: 0.

The audit covers Iramix C++ callback code. It does not claim that Windows driver
or WASAPI internals perform no allocation or locking outside that callback
scope.

## Dropout screening

- hard callback deadline misses: 0;
- target-duration misses: 0;
- event wait timeouts: 0;
- late wakeups above 1.5× the requested period: 1.

The single late wakeup is retained as a screening signal. This probe does not
perform acoustic loopback verification, so it must not be reported as proof of
zero audible dropouts.

## Raw process output

```text
audio_probe backend=WASAPI_shared sample_rate=48000 seconds_per_buffer=600 callback_workload=stereo_gain_silence
buffer=64 status=error message="IAudioClient::Initialize(exclusive) failed with HRESULT 2290679840"
buffer=128 status=error message="IAudioClient::Initialize(exclusive) failed with HRESULT 2290679840"
buffer=256 status=measured backend=WASAPI_exclusive stream_buffer=256 period_min=512 period_max=512 period_fundamental=512 callbacks=112498 p50_ms=0.002900 p95_ms=0.005100 p99_ms=0.005800 max_ms=0.301700 target_misses=0 hard_deadline_misses=0 late_wakeups=1 wait_timeouts=0 callback_allocations=0 callback_deallocations=0 callback_blocking_locks=0 mmcss=enabled
```

HRESULT `2290679840` is hexadecimal `0x88890020`,
`AUDCLNT_E_INVALID_DEVICE_PERIOD`.

## Remaining exit evidence

- obtain valid 64- and 128-frame streams on the Windows reference backend,
  then investigate the ASIO4ALL 64-frame cadence deficit on reference hardware;
- run equivalent Core Audio and Linux backend probes;
- repeat on release builds and declared reference machines;
- complete the full two-hour Week 2 soak;
- add acoustic or driver-level dropout verification.
