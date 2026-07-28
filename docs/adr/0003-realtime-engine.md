# ADR-0003: Immutable Real-time Render Plans

Status: Proposed; Tier-1 Windows live integration implemented  
Date: 2026-07-27

## Context

A DAW session changes frequently while the audio callback requires bounded and
predictable execution. Sharing a mutable graph between the UI and callback
would introduce locks, lifetime hazards, and hard-to-reproduce dropouts.

## Decision

- The editable session and executable audio graph are separate models.
- The graph compiler produces an immutable render plan.
- Plans are published atomically at safe buffer boundaries.
- Commands and parameter events use bounded lock-free queues.
- Destruction and memory reclamation occur outside real-time threads.
- Automation and modulation events carry sample offsets within the current
  process block.

## Validation required

- Allocation instrumentation reports zero allocations in the callback.
- Lock instrumentation reports no blocking synchronization in the callback.
- Repeated graph replacement produces no use-after-free under sanitizers.
- Latency compensation remains correct after routing changes.
- The engine survives command-queue saturation with explicit diagnostics.

## Initial implementation evidence

The first JUCE-free graph slice now implements the buffer and node ABI,
asymmetric bus metadata, transport snapshots, MIDI fan-in, graph compilation,
PDC, atomic plan publication, off-thread reclamation, and plan-owned node
lifetimes.

Windows MSVC and GCC/Ninja results are recorded in
[`../phase-0/results/REALTIME_GRAPH_WINDOWS_2026-07-27.md`](../phase-0/results/REALTIME_GRAPH_WINDOWS_2026-07-27.md).
The initial production nodes and WASAPI device-buffer integration are recorded
in
[`../phase-0/results/WASAPI_GRAPH_INTEGRATION_WINDOWS_2026-07-27.md`](../phase-0/results/WASAPI_GRAPH_INTEGRATION_WINDOWS_2026-07-27.md).
Bounded sample-accurate parameters and live WASAPI plan replacement are
recorded in
[`../phase-0/results/WASAPI_GRAPH_CONTROL_WINDOWS_2026-07-28.md`](../phase-0/results/WASAPI_GRAPH_CONTROL_WINDOWS_2026-07-28.md).
Bounded general commands, lossless completion backpressure, droppable
telemetry, queue saturation, and live WASAPI command completion are recorded
in
[`../phase-0/results/WASAPI_COMMAND_TELEMETRY_WINDOWS_2026-07-28.md`](../phase-0/results/WASAPI_COMMAND_TELEMETRY_WINDOWS_2026-07-28.md).
This evidence does not replace the two-hour callback soak, sanitizer,
ramp/modulation, edit-storm, denormal, or cross-platform evidence, so this ADR
remains proposed.
