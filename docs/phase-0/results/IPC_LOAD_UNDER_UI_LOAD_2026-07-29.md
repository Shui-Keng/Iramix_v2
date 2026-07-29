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

**The concurrent render thread's frame period is longer while that
traffic runs — consistently on Windows and Ubuntu, inconclusively on
hosted macOS.** That observation came out of investigating the flake.
Its magnitude is not reliably measurable here; see the section below
for the samples and for an earlier revision of this document that
over-claimed it.

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
measured the period under load and attributed it to the scene. The
warmed idle baseline shows the scene costing 1.7–4.5 ms per frame on
the environments where that baseline is stable. The difference between
the two is the subject of the section below, not a property of the
scene.

## Frame period with the IPC path idle and saturated

The task discards `WARMUP_FRAMES` and then samples `BASELINE_FRAMES`
with the IPC path unloaded, reporting that period next to the one
observed during the window. The pair is reported, never asserted
against a threshold — see "Why no ratio assertion" below.

Samples with a warmed baseline, one row per observed run:

| Environment | Idle | Under load | Factor | uiFrames |
|---|---:|---:|---:|---:|
| Ubuntu x64, CI | 2.019 ms | 2.596 ms | 1.3× | 67 |
| Windows 2022 x64, CI | 1.697 ms | 18.279 ms | 10.8× | 12 |
| Windows 2022 x64, CI | 1.785 ms | 7.519 ms | 4.2× | 20 |
| macOS arm64, CI | 4.527 ms | 23.555 ms | 5.2× | 8 |
| macOS arm64, CI | 11.451 ms | 11.041 ms | **1.0×** | 11 |
| macOS arm64, CI | 2.516 ms | 27.202 ms | 10.8× | 6 |
| Windows reference | 2.626 ms | 20.934 ms | 8.0× | 7 |
| Windows reference | 2.756 ms | 9.874 ms | 3.6× | 18 |
| Windows reference | 4.456 ms | 21.276 ms | 4.8× | 8 |
| Windows reference | 3.071 ms | 9.419 ms | 3.1× | 17 |

What these support:

- **Direction, on Windows and Ubuntu.** Every Windows sample, local and
  CI, shows a longer period under load, across an idle baseline that
  is itself stable at 1.7–4.5 ms. Ubuntu shows a small, consistent
  1.3× and keeps completing 44–67 frames per window.
- **Not the magnitude.** Two consecutive Windows CI runs give 10.8× and
  4.2×. The under-load figure is `window ÷ uiFrames`, so on a row with
  6–12 frames it carries very few significant digits.
- **Nothing conclusive on macOS.** The hosted macOS baseline still
  varies 2.5–11.5 ms after warmup, and one sample shows no effect at
  all (1.0×). The runner's own variance is the same size as the effect
  being measured.

**The cause is not established.** The scene is CPU raster with no GPU
involvement, so OS thread scheduling, JVM thread priority, and
per-platform blocking-stdio behaviour are all candidates. This task
separates none of them.

Note also that the idle baseline is not a machine-independent
constant: inside a full `gradle check` on the Windows reference
machine, sharing the host with the GPU and raster spikes, it rose to
17.276 ms. That is the intended behaviour — the baseline is a local
control measured under the same contention as the window, which is why
it is measured per run rather than assumed.

### A correction, and why the warmup exists

An earlier revision of this document reported factors of 1.6×/8.1×/
**29.6×** from a single three-OS run, and named the macOS figure as the
mechanism behind the macOS-only flake. That was over-claimed from one
sample and is withdrawn.

The next CI run contradicted it directly: macOS reported
`framePeriodIdle=21.800ms` against `framePeriodUnderLoad=12.030ms` — a
control slower than the load it was controlling for. The baseline was
sampling the render loop's first frames after startup, so it could
capture JIT and lazy initialization rather than steady-state cost. The
ping path had a 100-command warmup from the beginning; the render path
had none.

`WARMUP_FRAMES` discards 30 frames before the baseline sample. On the
Windows reference machine that took the idle baseline from swinging
2.7–17.3 ms to 2.6–4.5 ms across four consecutive runs. Every figure
in the table above is post-warmup; the pre-warmup numbers are not
comparable to them and are not reproduced here.

The retracted explanation of the macOS flake is not replaced by
another. macOS does complete far fewer frames per window than Ubuntu
(0–13 against 43–67), which is why it is the only environment whose
count can reach zero — but its baseline is also intrinsically slower
and noisier than Ubuntu's, and these samples cannot separate "slower
frame to begin with" from "degrades more under load".

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
[`30464246900`](https://github.com/Shui-Keng/Iramix_v2/actions/runs/30464246900),
the first with a warmed baseline, completed all five jobs
successfully:

| Environment | uiFrames | Evidence | Window | p50 | p95 | p99 | max |
|---|---:|---|---:|---:|---:|---:|---:|
| Ubuntu x64 | 67 | IN_WINDOW | 173.956 ms | 0.091 ms | 0.515 ms | 1.987 ms | 5.085 ms |
| Windows 2022 x64 | 12 | IN_WINDOW | 219.344 ms | 0.104 ms | 0.413 ms | 3.750 ms | 14.360 ms |
| macOS arm64 | 8 | IN_WINDOW | 188.437 ms | 0.046 ms | 0.255 ms | 1.020 ms | 79.553 ms |

Exact task output:

```text
IPC load test: os=Linux commands=1000 warmup=100 uiFrames=67 uiFrameEvidence=IN_WINDOW window=173.956ms baselineFrames=8 framePeriodIdle=2.019ms framePeriodUnderLoad=2.596ms p50=0.091ms p95=0.515ms p99=1.987ms max=5.085ms
IPC load test: os=Windows commands=1000 warmup=100 uiFrames=12 uiFrameEvidence=IN_WINDOW window=219.344ms baselineFrames=8 framePeriodIdle=1.697ms framePeriodUnderLoad=18.279ms p50=0.104ms p95=0.413ms p99=3.750ms max=14.360ms
IPC load test: os=macOS commands=1000 warmup=100 uiFrames=8 uiFrameEvidence=IN_WINDOW window=188.437ms baselineFrames=8 framePeriodIdle=4.527ms framePeriodUnderLoad=23.555ms p50=0.046ms p95=0.255ms p99=1.020ms max=79.553ms
```

The three environments use different toolchains (MSVC Debug, AppleClang
Debug, GCC Debug) and different runner hardware. The rows are **not** a
performance comparison between operating systems and must not be read
as one. The idle-versus-under-load *factor* within a single row is a
within-environment comparison and does not have that problem.

Two earlier runs are superseded and their figures are not comparable to
the table above: run
[`30460701335`](https://github.com/Shui-Keng/Iramix_v2/actions/runs/30460701335)
predates the baseline counters entirely, and run
[`30463235592`](https://github.com/Shui-Keng/Iramix_v2/actions/runs/30463235592)
has a cold baseline.

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
- that same loop completes frames more slowly while that traffic runs,
  on the Windows reference machine, Windows CI, and Ubuntu CI.

It does **not** prove:

- any UI frame rate or responsiveness figure — `uiFrames` is a
  liveness signal over a window only a few frame periods wide, and is
  not a frame-rate measurement;
- that the effect exists on hosted macOS. Its baseline still varies
  2.5–11.5 ms after warmup and one sample showed no effect at all, so
  the runner's variance is the size of the effect;
- *why* the render thread slows at all, or why the factor differs by
  operating system. OS thread scheduling, JVM thread priority, and
  per-platform blocking-stdio behaviour are all candidates; this task
  separates none of them;
- any particular factor. Two consecutive Windows CI runs gave 10.8×
  and 4.2×; the under-load period is `window ÷ uiFrames` over as few
  as six frames. The direction is evidence, the magnitude is not;
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
