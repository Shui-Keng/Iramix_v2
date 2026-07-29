# ADR-0003: Immutable Real-time Render Plans

Status: **Accepted for Windows Tier-1** on 2026-07-29, with two recorded
qualifications (see "Exit-review status" below): latency compensation
after routing changes is verified only on a graph-level fixture, and the
macOS and Linux backends are unmeasured.  
Date: 2026-07-27; accepted 2026-07-29

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
Five-topology graph edit-storm results on Windows and Linux, including
ASan/UBSan and TSan, are recorded in
[`../phase-0/results/GRAPH_EDIT_STORM_SANITIZERS_2026-07-28.md`](../phase-0/results/GRAPH_EDIT_STORM_SANITIZERS_2026-07-28.md).
Cross-block ramps, sample-rate modulation, and callback denormal protection
are recorded in
[`../phase-0/results/PARAMETER_RAMP_DENORMAL_2026-07-28.md`](../phase-0/results/PARAMETER_RAMP_DENORMAL_2026-07-28.md).
## Exit-review status — 2026-07-29

The two-hour callback soak named above has since been run, on two
independent Windows backends:
[`../phase-0/results/AUDIO_CALLBACK_SOAK_WINDOWS_2026-07-29.md`](../phase-0/results/AUDIO_CALLBACK_SOAK_WINDOWS_2026-07-29.md)
(ASIO4ALL, 128 and 256 frames) and
[`../phase-0/results/WASAPI_CALLBACK_SOAK_WINDOWS_2026-07-29.md`](../phase-0/results/WASAPI_CALLBACK_SOAK_WINDOWS_2026-07-29.md)
(WASAPI exclusive, 256 frames). 1,799,407 measured callbacks, zero target
misses, zero hard deadline misses, zero allocations, zero blocking locks.

Against the five validation criteria above: four are met, and *latency
compensation after routing changes* is **partial** — PDC is verified on a
graph-level fixture rather than the full plugin case, as
`REALTIME_GRAPH_WINDOWS_2026-07-27.md` says of itself.

The other condition in the original sentence, target-hardware backend
evidence for macOS and Linux, is not met and will not be met in Phase 0:
the project has no such hardware (R-13, accepted).

[`../phase-0/EXIT_REVIEW.md`](../phase-0/EXIT_REVIEW.md) therefore
recommends accepting this ADR for **Windows Tier-1**, carrying two
qualifications: PDC after routing changes is fixture-level only, and
macOS/Linux backends are unmeasured.

**Sign-off recorded 2026-07-29:** accepted on those terms as a project
decision. Both qualifications stand as written — acceptance does not
convert them into evidence. Closing them is tracked as P1-E2 (full plugin
PDC case) and P1-D1/P1-D3 (macOS and Linux backends, blocked by R-13).
If either turns out worse than the fixture suggests, this ADR is the
document that has to change, not the result documents.
