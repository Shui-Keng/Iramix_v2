# Skia GPU Device-loss Error-path Proxy — 2026-07-29

## Result

**The safe error-path proxy passes on the Windows reference machine.**
One `RenderException` injected inside Skiko's draw scope caused the
active backend to change from Direct3D to OpenGL. Skiko initialized a
replacement context and the harness observed completed presents from
that replacement context.

This result is deliberately narrower than native surface-allocation
failure and much narrower than real device loss. The task reports:

- `deviceLoss=SKIKO_RENDER_EXCEPTION_FALLBACK_PROXY`;
- `nativeSurfaceFailure=ACCEPTED_EVIDENCE_GAP`; and
- `literalTdr=ACCEPTED_EVIDENCE_GAP`.

## Why the failure is injected

The pinned Skiko 0.150.1 Direct3D path creates two GPU surfaces through
the private native `makeDirectXSurface` call. It has no public
fault-injection seam around that allocation:

- [`Direct3DContextHandler.initCanvas`](https://github.com/JetBrains/skiko/blob/v0.150.1/skiko/src/awtMain/kotlin/org/jetbrains/skiko/context/Direct3DContextHandler.kt#L30-L61)
  resizes/initializes the swap chain and creates both surfaces;
- [`Direct3DRedrawer.makeSurface`](https://github.com/JetBrains/skiko/blob/v0.150.1/skiko/src/awtMain/kotlin/org/jetbrains/skiko/redrawer/Direct3DRedrawer.kt#L90-L105)
  crosses directly into the native implementation; and
- [`SkiaLayer.inDrawScope`](https://github.com/JetBrains/skiko/blob/v0.150.1/skiko/src/awtMain/kotlin/org/jetbrains/skiko/SkiaLayer.awt.kt#L597-L613)
  catches `RenderException`, selects the next backend, and renders
  immediately with it.

Forcing the native call itself to fail would require at least one unsafe
or uncontrolled action: extreme surface dimensions/allocation pressure,
native-handle mutation, adapter removal, a driver reset, or TDR. None is
appropriate for the only development machine or an ordinary CI runner.

The safe proxy therefore injects exactly one managed `RenderException`
inside `SkiaLayer.inDrawScope`. This exercises Skiko's real
exception-to-fallback recovery handler without claiming that the native
allocator produced the exception.

## Harness

`GpuDeviceLossProxySpike` opens the same visible, continuously rendering
`SkiaLayer` used by the resize and context-recreation slices.
`GpuRecoveryFrame.exerciseDeviceLossProxy` then:

1. waits for the initial backend context and a completed baseline
   present;
2. records the initial backend and context identity;
3. invokes `SkiaLayer.inDrawScope` on the AWT event-dispatch thread and
   throws one controlled `RenderException`;
4. waits for `SkiaLayerAnalytics.contextInit()` from a newer context;
5. requires the reported backend to change; and
6. requires three render callbacks and three completed
   `afterFrameRender` observations matched to the replacement context
   and current surface dimensions.

The ten-second bounded progress deadline used by the previous recovery
slices remains active, so a crash, exception, or recovery hang fails the
task rather than silently passing.

The proxy does not:

- allocate an oversized surface;
- exhaust JVM heap, native memory, or VRAM;
- call a driver-reset or TDR control;
- remove an adapter;
- use reflection to corrupt Skiko or Direct3D handles; or
- modify the pinned Skiko binary.

## Windows reference-machine result

Command:

```text
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/gradle.ps1 :ui:desktop:gpuDeviceLossProxySpike --rerun-tasks
```

Observed output:

```text
[SKIKO] warn: Exception in draw scope
org.jetbrains.skiko.RenderException: Injected safe proxy for a backend surface-allocation RenderException.
GPU device-loss proxy: target=windows-x64 initialBackend=DIRECT3D recoveredBackend=OPENGL injectedFailures=1 contextInitializations=1 renderCallbacks=6 completedPresents=6 deviceLoss=SKIKO_RENDER_EXCEPTION_FALLBACK_PROXY nativeSurfaceFailure=ACCEPTED_EVIDENCE_GAP literalTdr=ACCEPTED_EVIDENCE_GAP
```

The OpenGL result is a fallback, not restoration of Direct3D. The
important completion evidence is that the fallback context initialized
and produced completed backend presents after the controlled error.

## CI integration

`gpuDeviceLossProxySpike` is part of `gradle check` after the existing
GPU context-recreation task. It needs a display; Linux CI therefore runs
it under Xvfb like the other GPU tasks.

Hosted runner results are not recorded until the branch has actually
run in CI.

## Accepted evidence gaps

### Native Direct3D surface-allocation failure

**Status: accepted as a Phase 0 evidence gap on 2026-07-29.**

The proxy proves Skiko's managed `RenderException` fallback path and
post-fallback rendering. It does not prove that a failure returned by
`makeDirectXSurface`, swap-chain resize, or native flush is translated
and recovered identically. Closing this gap needs either an upstream
Skiko/native fault-injection seam or an isolated disposable GPU test
machine where allocation failure can be induced without risking the
development host.

### Literal device loss / TDR

**Status: accepted as a Phase 0 evidence gap on 2026-07-29.**

No DXGI device removal, driver reset, watchdog timeout, or TDR occurred.
Those events can disrupt the desktop session, other GPU clients, CI
agent, and logging. They are not safe automated gates on the available
machine.

Closing this gap requires a controlled disposable or dedicated Windows
GPU machine, out-of-band observability, and a recovery runbook. The
result must separately prove whether Skiko restores Direct3D, falls back
to another backend, or terminates.

## Evidence boundary

This slice proves only that:

- the pinned Skiko error handler observes a controlled
  `RenderException`;
- it disposes the active Direct3D path and selects an available fallback
  on the Windows reference machine;
- the fallback creates a distinguishable context; and
- rendering and completed presents resume on that context.

It does **not** prove:

- native Direct3D surface-allocation failure;
- `DXGI_ERROR_DEVICE_REMOVED`, device reset, or TDR recovery;
- recovery back to Direct3D rather than fallback to OpenGL;
- preservation of application GPU resources across a real device loss;
- a real Linux GPU backend; or
- device-loss behavior on physical macOS/Linux hardware.
