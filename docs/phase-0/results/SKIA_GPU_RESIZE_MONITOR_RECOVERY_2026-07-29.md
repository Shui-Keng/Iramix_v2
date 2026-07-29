# Skia GPU Resize and Monitor-Move Recovery — 2026-07-29

Status: P0-005 Week 4 slice 2 (R-03). Resize recovery has measured
Windows evidence. Monitor-move recovery does not: the development machine
exposes only one AWT graphics device, so that half of the slice remains a
declared hardware limitation rather than being marked done.

## Scope

`GpuRecoverySpike` opens a visible, undecorated `JFrame` containing a real
`SkiaLayer`, then leaves its render loop running while the frame moves
through nine logical sizes:

```text
960x600, 640x400, 1200x750, 320x200, 1024x640,
800x500, 1152x720, 480x300, 640x400
```

The first size establishes the starting surface and the remaining eight
are real size transitions, including repeated shrink/grow cycles and a
return to 640x400 after other sizes.

An `onRender` callback is not by itself counted as recovery evidence:
Skiko records that callback into a picture before the selected backend
draws it. The harness supplies `SkiaLayerAnalytics` and requires three
`afterFrameRender` callbacks at every target size. In pinned Skiko
0.150.1, that callback follows the backend draw/present body. On the
Direct3D path, a size change closes the old two-buffer Skia surfaces,
resizes the swap-chain buffers, and creates replacement surfaces before
the draw can complete. A stage therefore passes only after:

1. AWT reports the new `SkiaLayer` size and content scale.
2. `onRender` receives the matching physical-pixel dimensions.
3. At least three backend draw/present bodies return successfully.

The callback schedules the next frame throughout the sequence, so this is
not stop-render, resize, then restart-render.

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\gradle.ps1 :ui:desktop:gpuRecoverySpike --rerun-tasks
```

`gpuRecoverySpike` is also wired into `gradle check` after `gpuSpike`.
The task will therefore run on the existing Windows/macOS/Linux CI
matrix; this document does not claim those new CI runs have happened yet.

## Local result

Toolchain: Eclipse Temurin 21.0.11+10, Gradle 9.6.1, Skiko 0.150.1
(`skiko-awt-runtime-windows-x64`), Windows, Direct3D. The same development
machine and hardware described by the slice 1 frame-time result were used.

```text
GPU recovery: target=windows-x64 backend=DIRECT3D resizeStages=9 resizeTransitions=8 renderCallbacks=46 completedPresents=45 sizes=[960x600->960x600@1.00;640x400->640x400@1.00;1200x750->1200x750@1.00;320x200->320x200@1.00;1024x640->1024x640@1.00;800x500->800x500@1.00;1152x720->1152x720@1.00;480x300->480x300@1.00;640x400->640x400@1.00] monitorCount=1 monitorVisited=0 monitorPresents=0 monitorMove=LIMITATION_SINGLE_MONITOR
```

All nine requested sizes reached their exact expected pixel size on this
1.00-scale display. The continuously running loop delivered 46 scene
callbacks and 45 completed Direct3D presents during the resize stages.
The process returned normally: no native crash, Java exception, stage
timeout, or stuck Gradle process was observed.

This is evidence that the Direct3D surface/swap-chain resize path recovered
after all eight size transitions. It is stronger than counting AWT resize
events or picture-recording callbacks, because each stage waits for
backend work to return after the new target dimensions were observed.

The task then completed again inside a forced full `gradle check` run:

```text
GPU recovery: target=windows-x64 backend=DIRECT3D resizeStages=9 resizeTransitions=8 renderCallbacks=47 completedPresents=46 sizes=[960x600->960x600@1.00;640x400->640x400@1.00;1200x750->1200x750@1.00;320x200->320x200@1.00;1024x640->1024x640@1.00;800x500->800x500@1.00;1152x720->1152x720@1.00;480x300->480x300@1.00;640x400->640x400@1.00] monitorCount=1 monitorVisited=0 monitorPresents=0 monitorMove=LIMITATION_SINGLE_MONITOR
```

The complete Java verification graph finished `BUILD SUCCESSFUL` with all
six actionable tasks executed. Callback totals may exceed the required
three per stage because the loop remains live and Skiko may schedule an
additional frame around a resize; the invariant is the completed
per-stage minimum, not an exact global callback count.

## Monitor-move result: limitation, not completion

The JVM reported:

```text
monitorCount=1 monitorVisited=0 monitorPresents=0
monitorMove=LIMITATION_SINGLE_MONITOR
```

There is no second physical or virtual `GraphicsDevice` on this machine.
Moving the frame to another coordinate on the same device would not test a
monitor transition, graphics-configuration replacement, cross-monitor
content scale, or adapter recovery. The harness deliberately does not do
that and call it success.

When two or more devices are available, the same task centers the live
window on every device, verifies that the frame's observed
`GraphicsDevice` ID changed to the requested device, recalculates the
physical target using that device's content scale, and requires three
completed backend presents there. Only that path prints
`VERIFIED_MULTI_MONITOR`.

## Evidence boundary

This proves continuous dense-scene rendering and completed Direct3D
presents across eight real `JFrame` size transitions on one Windows
machine. It also proves the automated task fails on a per-stage 10-second
deadline rather than hanging indefinitely.

It does not prove:

- **monitor-move recovery.** Only one `GraphicsDevice` exists on the
  development machine, so no cross-monitor movement happened. The output
  says `LIMITATION_SINGLE_MONITOR`; this item remains open;
- **HiDPI or mixed-DPI behavior.** Every local observation reported
  `@1.00`. Physical and logical dimensions matching is evidence of the
  absence of scaling here, not evidence that scaling works (R-07);
- **recovery on macOS Metal, Linux OpenGL, or Linux's CI software
  fallback.** The task is in the three-OS `check` graph, but this result
  records only the local Windows Direct3D run. CI must run before those
  paths can be claimed;
- **native surface object identity.** The public Skiko API exposes
  completed backend-frame analytics but not its internal `Surface`
  pointer. The evidence is successful backend draw/present after each
  dimension transition, joined with pinned Skiko's recreation path, not
  a direct pointer comparison;
- **sleep/wake or device-loss recovery.** Neither event was injected.
  Those remain separate Week 4 slices;
- **GPU pixel identity.** The recovery task draws the reference scene but
  does not read back or compare GPU pixels. `RasterSpike` remains the
  pixel-baseline evidence.
