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

**The concurrent render thread is starved by that traffic, by 1.6× on
Ubuntu, 8.1× on Windows, and 29.6× on macOS.** That finding came out
of investigating the flake and is the more consequential half of this
document.

## What the frame counter does and does not evidence

`uiFrames` counts frames the dummy render thread *completed* inside the
measurement window. It is easy to read as a UI-responsiveness figure. It
is not one, and the measurements below are the reason.

The window is the wall time of 1,000 pings — 85–376 ms observed. The
frame period under that load is 3–56 ms depending on environment, so
the window is only a handful of frame periods wide, and on the
shortest observed macOS window (85 ms) it is barely two.

Consequently `uiFrames` swings between 0 and 50 on identical code. The
counter is retained because a wedged render thread is still worth
catching, but **it is a liveness signal, not a frame-rate
measurement**, and it must not be quoted as one. The `window=` field is
printed alongside it so the margin is visible without re-deriving it
from CI.

An earlier revision of this document inferred the frame *cost* from
`window ÷ uiFrames` and reported 14–40 ms. That was wrong: it
measured the period under load and attributed it to the scene. The idle
baseline added on 2026-07-29 shows the scene actually costs 1.4–1.9 ms
per frame. The difference between those two numbers is the finding
below, not a property of the scene.

## The render thread starves under saturated IPC traffic

The task samples `BASELINE_FRAMES` frames with the IPC path unloaded
before the window opens, and reports that period next to the one
observed during the window. The pair is reported, never asserted
against a threshold — see "Why no ratio assertion" below.

Push run
[`30463235592`](https://github.com/Shui-Keng/Iramix_v2/actions/runs/30463235592):

| Environment | Idle period | Under-load period | Factor | uiFrames |
|---|---:|---:|---:|---:|
| Ubuntu x64 | 1.859 ms | 3.053 ms | 1.6× | 50 |
| Windows 2022 x64 | 1.370 ms | 11.153 ms | 8.1× | 18 |
| macOS arm64 | 1.887 ms | 55.777 ms | 29.6× | 3 |

The idle cost of the scene is effectively the same on all three
environments. What differs by an order of magnitude is how much of the
render thread survives a control thread issuing 1,000 back-to-back
blocking pings.

This is the mechanism behind the macOS-only flake documented below.
The frame counter never failed on Ubuntu or Windows because their
render loops keep completing 18–50 frames per window; macOS completes
0–13, so it is the only environment where the count can reach zero.
The original assertion was not measuring an IPC defect — it was
sampling a scheduling difference too finely to be stable.

**The cause of that difference is not established here.** The scene is
CPU raster with no GPU involvement, so the candidates are OS thread
scheduling, JVM thread priority, and the blocking-read behaviour of the
stdio transport on each platform. This task distinguishes none of them.

Note also that the idle baseline is not a machine-independent constant:
inside a full `gradle check` on the Windows reference machine, sharing
the host with the GPU and raster spikes, it rose from ~3 ms to
17.276 ms. That is the intended behaviour — the baseline is a local
control measured under the same contention as the window, which is why
it is measured per run rather than assumed.

### Why no ratio assertion

Reporting these as counters rather than asserting a ratio is
deliberate. The under-load period is `window ÷ uiFrames`, and
`uiFrames` can legitimately be 1 — or 0, in which case the task
reports `framePeriodUnderLoad=UNRESOLVED` rather than a fabricated
number. A
threshold over a quantity that coarse would reintroduce exactly the
flakiness this task had just removed, in a form that is harder to
diagnose. Interpreting the pair is left to the reader.

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
2. a bounded 10 s sample of 8 baseline frames, which both proves the
   render loop is advancing and measures the idle frame period;
3. 1,000 measured pings, each timed individually;
4. the in-window frame delta, with the bounded drain above if it is
   zero.

The baseline period is divided by the frame count actually observed,
not by the requested 8, since the loop can complete further frames
between the wake-up and the read.

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
IPC load test: os=Windows commands=1000 warmup=100 uiFrames=6 uiFrameEvidence=IN_WINDOW window=174.475ms baselineFrames=8 framePeriodIdle=2.754ms framePeriodUnderLoad=29.079ms p50=0.093ms p95=0.376ms p99=1.824ms max=14.592ms
IPC load test: os=Windows commands=1000 warmup=100 uiFrames=7 uiFrameEvidence=IN_WINDOW window=199.873ms baselineFrames=8 framePeriodIdle=3.591ms framePeriodUnderLoad=28.553ms p50=0.103ms p95=0.434ms p99=1.725ms max=11.091ms
IPC load test: os=Windows commands=1000 warmup=100 uiFrames=10 uiFrameEvidence=IN_WINDOW window=181.548ms baselineFrames=8 framePeriodIdle=3.809ms framePeriodUnderLoad=18.155ms p50=0.097ms p95=0.461ms p99=1.615ms max=14.626ms
IPC load test: os=Windows commands=1000 warmup=100 uiFrames=8 uiFrameEvidence=IN_WINDOW window=241.956ms baselineFrames=8 framePeriodIdle=3.447ms framePeriodUnderLoad=30.245ms p50=0.077ms p95=0.518ms p99=2.249ms max=32.081ms
```

Within a full `gradle check`, sharing the machine with the GPU and
raster spikes, the same task reported a wider tail — and an idle
baseline five times higher, since the baseline records that contention
too:

```text
IPC load test: os=Windows commands=1000 warmup=100 uiFrames=7 uiFrameEvidence=IN_WINDOW window=187.225ms baselineFrames=8 framePeriodIdle=17.276ms framePeriodUnderLoad=26.746ms p50=0.091ms p95=0.333ms p99=1.862ms max=25.041ms
```

The median ping latency is stable under that contention; the tail is
not. No attribution for the tail was attempted — GC counters are not
collected by this task, unlike the raster and GPU spikes.

Earlier runs on this machine, before the baseline counters existed,
reported `uiFrames` of 20/11/14/10 over comparable windows. Those runs
are not comparable to the four above: sampling eight baseline frames
before the window shifts where the loop is in its cycle when the pings
start.

## CI integration

`ipcLoadTest` is part of `gradle check` on all three OS build jobs, each
with `IRAMIX_ENGINE_PROBE` pointing at the engine probe built earlier in
the same job. Push run
[`30463235592`](https://github.com/Shui-Keng/Iramix_v2/actions/runs/30463235592)
completed all five jobs successfully:

| Environment | uiFrames | Evidence | Window | p50 | p95 | p99 | max |
|---|---:|---|---:|---:|---:|---:|---:|
| Ubuntu x64 | 50 | IN_WINDOW | 152.648 ms | 0.099 ms | 0.198 ms | 2.181 ms | 4.326 ms |
| Windows 2022 x64 | 18 | IN_WINDOW | 200.761 ms | 0.121 ms | 0.320 ms | 1.646 ms | 20.975 ms |
| macOS arm64 | 3 | IN_WINDOW | 167.332 ms | 0.047 ms | 0.239 ms | 2.504 ms | 55.985 ms |

Exact task output:

```text
IPC load test: os=Linux commands=1000 warmup=100 uiFrames=50 uiFrameEvidence=IN_WINDOW window=152.648ms baselineFrames=8 framePeriodIdle=1.859ms framePeriodUnderLoad=3.053ms p50=0.099ms p95=0.198ms p99=2.181ms max=4.326ms
IPC load test: os=Windows commands=1000 warmup=100 uiFrames=18 uiFrameEvidence=IN_WINDOW window=200.761ms baselineFrames=8 framePeriodIdle=1.370ms framePeriodUnderLoad=11.153ms p50=0.121ms p95=0.320ms p99=1.646ms max=20.975ms
IPC load test: os=macOS commands=1000 warmup=100 uiFrames=3 uiFrameEvidence=IN_WINDOW window=167.332ms baselineFrames=8 framePeriodIdle=1.887ms framePeriodUnderLoad=55.777ms p50=0.047ms p95=0.239ms p99=2.504ms max=55.985ms
```

The three environments use different toolchains (MSVC Debug, AppleClang
Debug, GCC Debug) and different runner hardware. The rows are **not** a
performance comparison between operating systems and must not be read
as one. The idle-versus-under-load *factor* within a single row is a
within-environment comparison and does not have that problem.

The preceding run
[`30460701335`](https://github.com/Shui-Keng/Iramix_v2/actions/runs/30460701335),
before the baseline counters existed, reported `uiFrames` of 43
(Ubuntu), 13 (Windows), and 11 (macOS) over 109–217 ms windows.

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
- median round-trip latency is 0.047–0.127 ms in Debug builds under a
  concurrent Skia raster load;
- the dummy render loop is not wedged by that IPC traffic; and
- that same loop is nonetheless slowed by it, by a factor that differs
  per environment and reached 29.6× on hosted macOS.

It does **not** prove:

- any UI frame rate or responsiveness figure — `uiFrames` is a
  liveness signal over a window only a few frame periods wide, and is
  not a frame-rate measurement;
- *why* the starvation factor differs so widely by operating system.
  OS thread scheduling, JVM thread priority, and per-platform blocking
  stdio behaviour are all candidates; this task separates none of them;
- that the starvation factors are precise. The under-load period is
  `window ÷ uiFrames`, so the macOS figure rests on three frames and
  the Ubuntu figure on fifty. Treat the macOS number as an order of
  magnitude, not a measurement;
- that a paced or batched command stream starves the render thread at
  all. 1,000 back-to-back blocking pings is a deliberately pathological
  control-thread pattern, not a model of real UI traffic;
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
