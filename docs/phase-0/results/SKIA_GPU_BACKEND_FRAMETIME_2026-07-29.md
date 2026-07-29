# Skia GPU Backend and Frame-Time Baseline — 2026-07-29

Status: P0-005 Week 4 slice 1 (R-03), and the first evidence of any kind
for the hardware-accelerated Skiko path. Until now `IramixDesktop`
opened a `SkiaLayer` window that ran but was never measured: no backend
identification, no frame-time trace, no result document. This closes
that gap for the first exit item Week 4 lists — "Record the actual...
rendering backends selected" — and produces the first GPU frame-time
trace.

## Scope

`GpuSpike` drives the same `RasterScene` used by the Week 3 CPU raster
baseline, but through a real, visible, undecorated `SkiaLayer` window
instead of an off-screen `Surface.makeRasterN32Premul`. Frames are
scheduled through Skiko's own redraw path — each `onRender` callback
records a timestamp and requests the next frame via
`SwingUtilities.invokeLater`, rather than being called synchronously in
a loop — so what is measured is frame-to-frame cadence as Skiko and the
GPU driver actually produce it, not the cost of one call. 60 frames are
discarded as warmup and 200 are measured, matching `RasterSpike`'s
warmup/measurement split.

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\gradle.ps1 :ui:desktop:gpuSpike
```

The task is wired into `gradle check` alongside `rasterSpike`. It does
**not** set `java.awt.headless=true` — it needs a real or virtual
display, which is the opposite requirement from `rasterSpike`.

## Local result

Toolchain: Eclipse Temurin 21.0.11+10, Gradle 9.6.1, Skiko 0.150.1
(`skiko-awt-runtime-windows-x64`), HotSpot default GC. Hardware: AMD
Athlon Silver 3050U, 2 cores. Windows-only per R-13.

```text
GPU spike: target=windows-x64 backend=DIRECT3D size=1440x900
logical=1440x900 frames=200 p50=16.604ms p95=18.151ms p99=27.884ms
max=32.206ms gcCount=4 gcMillis=16
```

**The backend Skiko actually selected on this host is Direct3D**,
reported through `SkiaLayer.getRenderApi()` rather than assumed from
the platform. `size` equals `logical` exactly: this development
machine reported no display scale, so pixel size and logical size
coincide here, and that equality should not be read as HiDPI having
been exercised — see the evidence boundary.

### The tail widens under concurrent load

The same run captured as part of a full `gradle check` pass — where
`rasterSpike`'s CPU work runs shortly afterward in the same JVM
session — showed a materially wider tail:

```text
GPU spike: target=windows-x64 backend=DIRECT3D size=1440x900
logical=1440x900 frames=200 p50=16.652ms p95=35.765ms p99=66.556ms
max=132.548ms gcCount=4 gcMillis=19
```

p50 is stable (16.6ms both times) but p99 moved from 27.9ms to 66.6ms
and max from 32.2ms to 132.5ms. This is recorded rather than
smoothed over: on this two-core laptop, GPU frame delivery is
sensitive to what else the host is doing, and the isolated run above
should not be read as a guaranteed tail.

## Against a frame budget

A 60Hz budget is 16.67ms and 120Hz is 8.33ms.

| Run | p50 | p50 vs 60Hz | p99 vs 60Hz | p99 vs 120Hz |
|---|---:|---:|---:|---:|
| Isolated | 16.60ms | at budget | 167% (misses) | 335% (misses) |
| Under concurrent `check` load | 16.65ms | at budget | 399% (misses) | 799% (misses) |

**p50 sits almost exactly on the 60Hz line and 120Hz is missed even at
the median.** This is a real GPU path (Direct3D, not software), so
unlike the Week 3 CPU number this is not explained by the absence of
hardware acceleration; on this integrated/laptop GPU, Skiko's default
present path does not clear a 60Hz budget with room to spare. Whether
that is vsync pacing, present-mode overhead, or driver behavior on this
specific hardware is not established here — only the counter is.

## Evidence boundary

This proves that a real `SkiaLayer` window opens, selects Direct3D on
this Windows host, renders the Week 3 reference scene without crashing
across 200 scheduled frames, and produces a frame-time distribution
that sits at the 60Hz budget line at the median and misses it at the
tail.

It does not prove:

- **anything about macOS or Linux.** No `MetalRedrawer` or
  `LinuxOpenGLRedrawer` path has run. Which backend Skiko selects on
  those platforms — and whether Linux CI, which has no display by
  default, can exercise this at all without a virtual display such as
  Xvfb — is still open;
- **anything about resize, monitor move, sleep/wake, or device loss.**
  The window is created once at a fixed size and never touched again.
  These are the remaining Week 4 exit items and are explicitly deferred
  to later slices, not attempted here;
- **anything about HiDPI.** `size` matched `logical` because this host
  reported no scale factor; a scaled display was not exercised, and R-07
  stays open;
- **that the frame-time number is representative.** Two runs on one
  two-core laptop produced the same median and very different tails.
  Nothing here establishes what a quieter or a busier host would show,
  and no other hardware has been measured (R-13);
- **that GPU pixel output matches the CPU raster baseline, or is stable
  across runs.** No screenshot comparison is attempted here by design —
  GPU anti-aliasing and blending are permitted to differ from the CPU
  path and across vendors, so a byte-identity check would not be
  measuring anything stable. `RasterSpike`'s baseline remains the only
  pixel-correctness evidence Phase 0 has;
- **that CI can run this at all.** The task is wired into `gradle
  check`, but has not yet run there. Windows and macOS hosted runners
  are expected to have an interactive session; Linux hosted runners have
  none by default and will need a virtual display before this task can
  produce anything but a headless skip on that leg.
