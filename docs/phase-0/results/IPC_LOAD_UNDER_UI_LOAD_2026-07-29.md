# IPC Load Under Dummy Skia UI Load — 2026-07-29

## Result

**1,000 sequential IPC commands complete against a live engine process
on the Windows reference machine and all three hosted CI operating
systems, while a Skia raster render thread runs concurrently.** Median
round-trip is 0.061–0.127 ms across environments.

This document exists because `ipcLoadTest` had been running in
`gradle check` since P0-007 without a recorded result. Its counter line
is the deliverable and had never been captured, so the "load smoke
green" claim on the task board carried no numbers.

It also records a measurement defect found on 2026-07-29: the task's UI
frame assertion was racy, and the fix narrowed what that assertion is
allowed to claim.

## What the frame counter does and does not evidence

`uiFrames` counts frames the dummy render thread *completed* inside the
measurement window. It is easy to read as a UI-responsiveness figure. It
is not one, and the measurements below are the reason.

The window is the wall time of 1,000 pings — 85–376 ms observed. A
completed raster frame of the 200-track scene costs roughly 20–40 ms
on macOS CI and 14–27 ms on the Windows reference machine. The window
is therefore only a handful of frame periods wide, and on the shortest
observed macOS window (85 ms) it is barely two.

Consequently `uiFrames` swings between 0 and 43 on identical code,
driven by runner scheduling rather than by anything the IPC path does.
The counter is retained because a wedged render thread is still worth
catching, but **it is a liveness signal, not a frame-rate measurement**,
and it must not be quoted as one. The `window=` field is printed
alongside it so the margin is visible without re-deriving it from CI.

## The race and what changed

`IpcLoadTest` asserted `uiFrames != 0` over the measurement window. On
macOS CI this failed intermittently with
`AssertionError: Dummy Skia UI load did not render during measurement.`

The failure was confirmed as a race rather than a regression before any
change was made: commit `978afd0` produced two workflow runs from
identical code — run
[`30458579310`](https://github.com/Shui-Keng/Iramix_v2/actions/runs/30458579310)
(`pull_request`) passed macOS, and run
[`30458580007`](https://github.com/Shui-Keng/Iramix_v2/actions/runs/30458580007)
(`push`) failed macOS twice, including an explicit
`gh run rerun --failed`. Windows and Ubuntu were unaffected.

Two defects were involved, and the first fix only addressed one.

**Defect 1 — the loop was alive but not proven to be advancing.**
`DummyUiLoad` already blocked construction until the first frame, so the
render loop was live when the window opened. Nothing proved it had moved
*past* that frame, so the cost of frame two (lazy Skia initialization,
JIT, thread scheduling) landed inside the window.

The one-shot `firstFrame` latch was generalized into a `frameSignal`
monitor plus a bounded `awaitFrameAfter(baseline, timeout)`, used by the
constructor and by `IpcLoadTest` to wait for one further frame before
capturing the baseline.

**Defect 2 — the window is too short for the assertion.** CI disproved
the sufficiency of the first fix: the macOS leg of run
[`30460167859`](https://github.com/Shui-Keng/Iramix_v2/actions/runs/30460167859)
passed with `uiFrames=2`. Passing by two frames is one scheduler stall
away from failing, however live the loop is.

The assertion now still requires a frame, but no longer requires it to
*complete* inside the window. When the in-window delta is zero, the task
waits a bounded 5 s for the frame that was in flight during the window.
A render loop actually wedged by IPC load still fails, with a message
distinct from the never-started case. That outcome is reported as
`uiFrames=0 uiFrameEvidence=IN_FLIGHT_DRAINED` rather than counting a
frame that finished after the window closed.

## Harness

`IpcLoadTest` runs only when `IRAMIX_ENGINE_PROBE` is set; otherwise it
prints a skip line and exits. It launches a real `iramix_engine_probe`
child process through `EngineSession` and, concurrently, a `DummyUiLoad`
platform thread rendering a 1920×1080 raster scene of 200 tracks ×
10 clips through Skiko 0.150.1.

The sequence is:

1. 100 warmup pings, discarded;
2. a bounded 10 s wait for the render loop to advance one frame;
3. 1,000 measured pings, each timed individually;
4. the in-window frame delta, with the bounded drain above if it is
   zero.

Latency percentiles are nearest-rank over 1,000 samples, so p99 has ten
samples above it — unlike the 20-sample spikes elsewhere in this
directory, p99 here is not merely the observed maximum. `max` is
reported separately.

Both processes are **Debug** builds. These figures are transport-shape
evidence, not optimized-build latency.

## Windows reference-machine result

Toolchain: MSVC 19.44.35228.0, `windows-msvc` preset, Debug; Temurin
21.0.11+10; Gradle 9.6.1.

Command:

```text
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/gradle.ps1 :ui:desktop:ipcLoadTest --rerun-tasks -PiramixEngineProbe=build\windows-msvc\Debug\iramix_engine_probe.exe
```

Observed output over four consecutive runs:

```text
IPC load test: os=Windows commands=1000 warmup=100 uiFrames=20 uiFrameEvidence=IN_WINDOW window=207.785ms p50=0.104ms p95=0.297ms p99=2.491ms max=17.469ms
IPC load test: os=Windows commands=1000 warmup=100 uiFrames=11 uiFrameEvidence=IN_WINDOW window=182.822ms p50=0.110ms p95=0.304ms p99=1.189ms max=15.151ms
IPC load test: os=Windows commands=1000 warmup=100 uiFrames=14 uiFrameEvidence=IN_WINDOW window=199.908ms p50=0.094ms p95=0.300ms p99=1.491ms max=19.659ms
IPC load test: os=Windows commands=1000 warmup=100 uiFrames=10 uiFrameEvidence=IN_WINDOW window=193.078ms p50=0.112ms p95=0.339ms p99=1.729ms max=17.791ms
```

Within a full `gradle check`, sharing the machine with the GPU and
raster spikes, the same task reported a materially wider tail:

```text
IPC load test: os=Windows commands=1000 warmup=100 uiFrames=14 uiFrameEvidence=IN_WINDOW window=375.642ms p50=0.101ms p95=0.806ms p99=5.348ms max=45.246ms
```

The median is stable under that contention; the tail is not. No
attribution for the tail was attempted — GC counters are not collected
by this task, unlike the raster and GPU spikes.

## CI integration

`ipcLoadTest` is part of `gradle check` on all three OS build jobs, each
with `IRAMIX_ENGINE_PROBE` pointing at the engine probe built earlier in
the same job. Push run
[`30460701335`](https://github.com/Shui-Keng/Iramix_v2/actions/runs/30460701335)
completed all five jobs successfully:

| Environment | uiFrames | Evidence | Window | p50 | p95 | p99 | max |
|---|---:|---|---:|---:|---:|---:|---:|
| Ubuntu x64 | 43 | IN_WINDOW | 109.373 ms | 0.077 ms | 0.187 ms | 0.934 ms | 2.229 ms |
| Windows 2022 x64 | 13 | IN_WINDOW | 182.279 ms | 0.127 ms | 0.212 ms | 0.667 ms | 2.076 ms |
| macOS arm64 | 11 | IN_WINDOW | 216.906 ms | 0.061 ms | 0.644 ms | 3.769 ms | 16.413 ms |

Exact task output:

```text
IPC load test: os=Linux commands=1000 warmup=100 uiFrames=43 uiFrameEvidence=IN_WINDOW window=109.373ms p50=0.077ms p95=0.187ms p99=0.934ms max=2.229ms
IPC load test: os=Windows commands=1000 warmup=100 uiFrames=13 uiFrameEvidence=IN_WINDOW window=182.279ms p50=0.127ms p95=0.212ms p99=0.667ms max=2.076ms
IPC load test: os=macOS commands=1000 warmup=100 uiFrames=11 uiFrameEvidence=IN_WINDOW window=216.906ms p50=0.061ms p95=0.644ms p99=3.769ms max=16.413ms
```

The three environments use different toolchains (MSVC Debug, AppleClang
Debug, GCC Debug) and different runner hardware. The rows are **not** a
performance comparison between operating systems and must not be read as
one.

## macOS rerun sample

The macOS job of the run above was rerun three times on identical code
to sample the variance directly:

| Attempt | uiFrames | Evidence | Window | p50 | max |
|---|---:|---|---:|---:|---:|
| initial | 11 | IN_WINDOW | 216.906 ms | 0.061 ms | 16.413 ms |
| 1 | 2 | IN_WINDOW | 85.460 ms | 0.067 ms | 1.069 ms |
| 2 | **0** | **IN_FLIGHT_DRAINED** | 141.065 ms | 0.087 ms | 10.662 ms |
| 3 | 6 | IN_WINDOW | 198.934 ms | 0.109 ms | 27.826 ms |

All four passed. Attempt 2 is the load-bearing observation: it hit the
zero-frame condition that produced the original `AssertionError`, so
that run would have failed both under the original code and under the
first fix. One sample in four reached zero, which puts the residual
flake rate of the first fix on the order of 25 % of macOS runs.

Four samples bound that rate only loosely. They do not establish the
residual flake rate of the current code below that resolution; they
establish that the drain path is reached in practice and that it works.

## Evidence boundary

This slice proves only that:

- 1,000 sequential ping commands complete over the Phase 0 stdio
  transport against a live engine child process, on the Windows
  reference machine and all three hosted CI operating systems;
- median round-trip latency is 0.061–0.127 ms in Debug builds under a
  concurrent Skia raster load; and
- the dummy render loop is not wedged by that IPC traffic.

It does **not** prove:

- any UI frame rate or responsiveness figure — `uiFrames` is a
  liveness signal over a window only a few frame periods wide, and is
  not a frame-rate measurement;
- optimized-build latency, since both processes are Debug builds;
- latency under concurrent audio-callback load, real plugin traffic, or
  payloads larger than a ping — only ping round-trip is exercised;
- throughput or pipelining, since commands are strictly sequential;
- behavior on physical macOS or Linux hardware, as opposed to hosted CI
  runners (see [`../RISK_REGISTER.md`](../RISK_REGISTER.md));
- that the residual flake rate of the current assertion is zero — four
  macOS samples cannot resolve a rate below roughly 25 %;
- that a real UI event loop, as opposed to a tight raster loop with
  `Thread.yield()`, schedules the same way against IPC traffic; and
- anything about the GC or scheduler attribution of the latency tail,
  which this task does not instrument.
