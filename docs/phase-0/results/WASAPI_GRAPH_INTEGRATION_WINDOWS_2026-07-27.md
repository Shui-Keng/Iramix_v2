# WASAPI Graph Integration Screening — Windows — 2026-07-27

## Purpose

This screening verifies that the immutable render-plan executor and initial
production nodes run inside the real WASAPI event-driven callback path. The
previous probe measured a standalone stereo-gain loop over dummy buffers and
released the device buffer as silent. The new workload runs:

```text
DeviceInputNode
    -> TrackNode
    -> GainNode
    -> MixerNode
    -> OutputNode
    -> RenderPlanExecutor::renderTo
    -> planar-to-device conversion
    -> IAudioRenderClient::ReleaseBuffer
```

The production-node fixture deliberately has no bound input, so it emits
silence without using `AUDCLNT_BUFFERFLAGS_SILENT`. The graph output is still
copied and converted into the actual WASAPI buffer, proving the render path
without playing a test tone through the user's speakers.

## Callback contract

All graph storage, node ownership, planar scratch, MIDI capacity, and device
conversion storage are prepared before `IAudioClient::Start`.

The measured callback scope includes:

- immutable-plan execution;
- production-node processing;
- graph output snapshot;
- copy into the planar device destination;
- float32 or PCM16 interleaving.

Global allocation/deallocation hooks and the tracked blocking-lock hook remain
active across that complete scope.

## Deterministic production-node test

The test binds a two-channel planar input and renders it through track trim,
stereo pan, gain, mixer trim, and the explicit output sink.

Raw output:

```text
Production node chain: channels=2, frames=4, allocations=0, deallocations=0, blocking_locks=0
```

It also verifies that track mute clears both output channels.

## Live WASAPI screening

- Sample rate: 48 kHz
- Requested duration: 15 seconds per supported buffer size
- Backend: WASAPI shared with exclusive fallback
- Thread class: MMCSS `Pro Audio`, critical priority
- Workload: `immutable_graph_production_nodes`

Raw output:

```text
buffer=64 status=unsupported_device_period backend=WASAPI_exclusive stream_buffer=0 period_min=512 period_max=512 period_fundamental=512
buffer=128 status=unsupported_device_period backend=WASAPI_exclusive stream_buffer=0 period_min=512 period_max=512 period_fundamental=512
buffer=256 status=measured backend=WASAPI_exclusive stream_buffer=256 period_min=512 period_max=512 period_fundamental=512 callbacks=2813 p50_ms=0.035900 p95_ms=0.067000 p99_ms=0.080500 max_ms=0.233200 target_misses=0 hard_deadline_misses=0 late_wakeups=1 wait_timeouts=0 callback_allocations=0 callback_deallocations=0 callback_blocking_locks=0 graph_blocks=2913 graph_generation=1 mmcss=enabled
```

The 100 extra graph blocks are callback warmups and are intentionally excluded
from the 2,813 measured callback durations.

For the supported 256-frame stream:

- p99 `0.080500 ms` is below the `3.73 ms` engine target;
- maximum callback work was `0.233200 ms`;
- target misses: `0`;
- hard deadline misses: `0`;
- callback allocations, deallocations, and blocking locks: `0`;
- one late wakeup was observed; this is device/OS cadence evidence and is not
  hidden by the low engine execution time.

The command exits with code 4 because 64- and 128-frame streams remain
unsupported by this endpoint. That is the expected probe contract and P0-008
remains open.

## What this does not prove

- It is a 15-second integration screening, not the mandatory two-hour soak.
- The original run used block-rate parameters. The next-day follow-up adds
  sample-offset scalar events; ramps and modulation streams remain pending.
- Device capture is implemented at the node boundary but this WASAPI probe is
  render-only, so a live input stream is not exercised.
- ASIO, Core Audio, JACK, PipeWire, and ALSA are not integrated with this
  executor yet.
- Hot graph replacement was tested on a dedicated audio thread, but was not
  performed during this WASAPI run.
- No macOS or Linux runtime evidence was produced.

This result closes the initial Windows executor-to-WASAPI integration slice,
not P0-008 or the full Week 5 gate.

The next-day follow-up adds sample-accurate bounded parameter events and a
control-thread plan replacement during live playback:
[`WASAPI_GRAPH_CONTROL_WINDOWS_2026-07-28.md`](WASAPI_GRAPH_CONTROL_WINDOWS_2026-07-28.md).
