# Skia GPU Context-Recreation Proxy — 2026-07-29

Status: P0-005 Week 4 slice 3 (R-03). Deterministic Skiko context
teardown/recreation is verified on the Windows Direct3D reference machine.
Literal operating-system sleep/wake is explicitly accepted as a Phase 0
evidence gap; this proxy is not labeled as proof of it.

## Why this is a proxy

Putting the host operating system to sleep from an ordinary automated test
would suspend the test process, its deadline, the CI agent, and often the
network session needed to observe recovery. A shared CI runner also does
not promise that a guest can suspend or resume its host. Forcing real
sleep from `gradle check` would therefore be unsafe on a development
machine and non-deterministic or impossible in hosted CI.

The realistic automated failure boundary is Skiko's graphics context.
`GpuContextRecoverySpike` uses the same visible `SkiaLayer` and dense
`RasterScene` as the earlier GPU slices, then repeats this sequence five
times on the AWT event-dispatch thread:

1. Call `SkiaLayer.dispose()`, which disposes the redrawer, backend
   context, recorded picture, and native hardware layer resources.
2. Remove and re-add the same layer to its visible `JFrame`. In pinned
   Skiko 0.150.1 this calls `addNotify()` and `init(recreation=true)`,
   creating a replacement redrawer and backend context.
3. Require a new `SkiaLayerAnalytics.contextInit()` identity.
4. Require at least three matching `onRender` callbacks and three
   `afterFrameRender` completions from that replacement context.

Each stage has a 10-second deadline. A replacement context that never
initializes or never presents therefore fails the task rather than hanging
the test run.

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\gradle.ps1 :ui:desktop:gpuContextRecoverySpike --rerun-tasks
```

The task is wired into `gradle check` after the resize/monitor recovery
task. Hosted CI results are pending at the time of this initial local
record and will be added only after those jobs run.

## Local result

Toolchain: Eclipse Temurin 21.0.11+10, Gradle 9.6.1, Skiko 0.150.1
(`skiko-awt-runtime-windows-x64`), Windows, Direct3D.

Isolated task:

```text
GPU context recovery: target=windows-x64 backend=DIRECT3D cycles=5 contextInitializations=5 renderCallbacks=32 completedPresents=31 backends=[DIRECT3D>DIRECT3D>DIRECT3D>DIRECT3D>DIRECT3D>DIRECT3D] sleepWake=CONTEXT_RECREATE_PROXY literalSleepWake=ACCEPTED_EVIDENCE_GAP
```

Forced full `gradle check`:

```text
GPU context recovery: target=windows-x64 backend=DIRECT3D cycles=5 contextInitializations=5 renderCallbacks=30 completedPresents=29 backends=[DIRECT3D>DIRECT3D>DIRECT3D>DIRECT3D>DIRECT3D>DIRECT3D] sleepWake=CONTEXT_RECREATE_PROXY literalSleepWake=ACCEPTED_EVIDENCE_GAP
```

The six backend entries are the initial Direct3D context plus five
replacement contexts. All five replacements emitted a distinct
`contextInit` event and completed at least three presents. The process
returned normally with no native crash, Java exception, stage timeout, or
stuck Gradle process. The complete Java verification graph finished
`BUILD SUCCESSFUL` with seven actionable tasks executed; the engine probe
was not supplied locally, so the unrelated IPC load task reported its
existing skip.

## Accepted literal sleep/wake gap

The machine-readable output deliberately contains both:

```text
sleepWake=CONTEXT_RECREATE_PROXY
literalSleepWake=ACCEPTED_EVIDENCE_GAP
```

The first value states what ran. The second prevents that result from
being promoted into a claim about real OS suspend/resume. Phase 0 accepts
literal sleep/wake as a controlled-hardware/manual evidence gap, following
the same discipline used for unavailable macOS/Linux reference hardware:
record the constraint and decision rather than manufacture an automated
substitute and call it equivalent.

Closing the literal gap later requires a controlled physical-machine run
that can suspend the OS, resume it out of band, and preserve independent
logs across the interruption.

## Evidence boundary

This proves that the application can dispose and recreate the same
SkiaLayer backend context five times and resume completed Direct3D
presents after each replacement, under bounded automated deadlines.

It does not prove:

- **literal OS sleep/wake recovery.** The OS, compositor, display session,
  JVM, and GPU driver were never suspended. This is an accepted Phase 0
  gap, not a completed test;
- **device-loss recovery.** No adapter removal, driver reset, TDR, or
  backend error was injected. That remains a separate Week 4 slice;
- **recovery on macOS Metal, Linux OpenGL, or Ubuntu's CI software
  fallback.** Those claims require the pending hosted CI run; a real
  Linux GPU remains unavailable even after CI;
- **adapter fallback.** Every local recreation selected Direct3D. The
  test records backend sequence but did not force Direct3D to fail and
  observe selection of another API;
- **GPU pixel identity.** The dense scene is rendered after every
  recreation, but no GPU readback is compared. `RasterSpike` remains the
  pixel-baseline evidence.
