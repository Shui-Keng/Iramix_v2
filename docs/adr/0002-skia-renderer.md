# ADR-0002: Java UI Process with Skiko/Skia

Status: Accepted  
Date: 2026-07-27

## Context

Iramix needs a dense, cross-platform interface with waveforms, automation,
modulation overlays, meters, and thousands of timeline objects. UI development
speed is a primary product requirement. The audio engine must remain hard
real-time and isolated from UI stalls or failures.

## Decision

- The production desktop UI is written in Java 21.
- Skiko provides the Skia renderer and AWT window integration.
- The Java UI and C++ engine run as separate processes.
- The Java process owns presentation state, interaction, accessibility, and
  command construction.
- The C++ process owns audio devices, MIDI, DSP, transport authority, plugins,
  disk streaming, and the executable session graph.
- Communication uses a versioned IPC protocol. Java object serialization and a
  broad JNI object bridge are forbidden.
- Native plugin editors are hosted by a dedicated native bridge and surfaced to
  the Java application through explicit platform handles.

## Non-goals

- Java never executes an audio callback.
- JVM garbage collection is not part of an audio deadline.
- The Java process does not load third-party audio plugins.
- Skiko objects do not cross the process boundary.
- UI code cannot mutate the C++ render graph directly.

## Acceptance criteria

- Java UI starts and renders a reference scene on all three operating systems.
- UI process termination does not stop an already-running engine probe.
- The handshake rejects unsupported protocol versions.
- Timeline pan and zoom maintain the Phase 0 frame budget.
- Text shaping, IME, accessibility, and HiDPI pass reference cases.
- Device loss or renderer recreation does not interrupt audio.
- A project snapshot is immutable after publication to the UI.

## Risks

- JVM startup and memory footprint must be budgeted.
- Excess allocation can cause visible UI pauses even though audio is isolated.
- IPC introduces schema, ordering, reconnect, and back-pressure complexity.
- Native plugin-window embedding is risky, especially on Linux Wayland.
- Skiko exposes Kotlin-first APIs; Java interop must be tested and wrapped
  behind Iramix-owned Java interfaces.

The former C++ Skia/SDL spike was retired when Java UI was selected. It is not
part of the production architecture.

