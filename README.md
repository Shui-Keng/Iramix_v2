# Iramix

Iramix is a cross-platform digital audio workstation for composers and sound
designers. Its product direction combines a linear arrangement, scene-based
composition, universal modulation, and fast contextual editing.

The project is currently in **Phase 0: product definition and technical
validation**. Production features are intentionally not being built until the
architecture and performance gates in [`docs/phase-0/PLAN.md`](docs/phase-0/PLAN.md)
are satisfied.

## Technical direction

- Java 21 UI process with Skiko/Skia
- C++20 real-time engine, without JUCE
- Windows, macOS, and Linux
- Gradle for the Java desktop process
- CMake for the C++ engine
- Versioned IPC commands and immutable snapshots between UI and engine
- Native C++ adapters for audio, MIDI, plugins, and OS integration

## Configure and test

Build and test the C++ engine:

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

On Windows, the Visual Studio preset works without first activating a compiler
shell:

```powershell
cmake --preset windows-msvc
cmake --build --preset windows-msvc
ctest --preset windows-msvc
```

Bootstrap the pinned Java toolchain and build the UI:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\gradle.ps1 check
powershell -ExecutionPolicy Bypass -File .\scripts\gradle.ps1 :ui:desktop:run
```

Run the complete Windows Phase 0 verification, including a persistent
Java-to-C++ IPC session:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\verify-phase0.ps1
```

Run a short audio callback screening pass:

```powershell
.\build\windows-msvc\Debug\iramix_audio_probe.exe --seconds-per-buffer 3
```

The default build has no ASIO SDK dependency. To build the optional Windows
ASIO Phase 0 probe, supply an external ASIO SDK 2.3 checkout:

```powershell
cmake --preset windows-msvc -DIRAMIX_ASIO_SDK_PATH=C:\path\to\ASIOSDK
cmake --build build\windows-msvc --config Release --parallel
.\build\windows-msvc\Release\iramix_asio_probe.exe --list-drivers
.\build\windows-msvc\Release\iramix_asio_probe.exe --driver "Driver Name" --seconds-per-buffer 200
```

The last command runs 200 seconds at each of 64, 128, and 256 frames: ten
minutes total. Iramix has selected Steinberg's proprietary ASIO license, but
the developer agreement is not yet signed; ASIO-enabled builds therefore
remain experimental/development-only and are excluded from public releases.

The same `iramix_audio_probe` target uses Core Audio on macOS and JACK on
Linux. Target-machine commands, evidence requirements, and the two-hour soak
schedule are in
[`docs/phase-0/AUDIO_PROBE_RUNBOOK.md`](docs/phase-0/AUDIO_PROBE_RUNBOOK.md).

The native graph spike now includes a JUCE-free planar buffer ABI, bus-aware
node contract, immutable render-plan compiler, graph-level delay
compensation, fixed-capacity MIDI, and acknowledgement-based lock-free plan
publication. The initial production layer adds device-input, track, gain,
mixer, and output nodes. Run its deterministic checks with:

```powershell
ctest --preset windows-msvc -R "iramix.audio_graph|iramix.plan_swap" -V
```

Initial scope, raw counters, and remaining gaps are recorded in
[`docs/phase-0/results/REALTIME_GRAPH_WINDOWS_2026-07-27.md`](docs/phase-0/results/REALTIME_GRAPH_WINDOWS_2026-07-27.md).
The executor now renders through the live WASAPI callback; the short Windows
integration result is in
[`docs/phase-0/results/WASAPI_GRAPH_INTEGRATION_WINDOWS_2026-07-27.md`](docs/phase-0/results/WASAPI_GRAPH_INTEGRATION_WINDOWS_2026-07-27.md).
The bounded sample-accurate parameter queue and live plan-swap result are in
[`docs/phase-0/results/WASAPI_GRAPH_CONTROL_WINDOWS_2026-07-28.md`](docs/phase-0/results/WASAPI_GRAPH_CONTROL_WINDOWS_2026-07-28.md).
The bounded general-command, lossless completion-backpressure, and droppable
telemetry result is in
[`docs/phase-0/results/WASAPI_COMMAND_TELEMETRY_WINDOWS_2026-07-28.md`](docs/phase-0/results/WASAPI_COMMAND_TELEMETRY_WINDOWS_2026-07-28.md).

See [`docs/adr/0002-skia-renderer.md`](docs/adr/0002-skia-renderer.md) and
[`docs/architecture/IPC_PROTOCOL.md`](docs/architecture/IPC_PROTOCOL.md) before
changing the process boundary.
