# Phase 0 Execution Plan

Target duration: 8 weeks  
Status: In progress  
Started: 2026-07-27

## Outcomes

Phase 0 ends with evidence that the chosen architecture can support a
professional DAW. It does not end with a collection of production features.

Required outputs:

- validated product brief and v1 scope;
- interaction prototypes for arrangement, mixer, launcher, and device panel;
- audio callback spike on all three operating systems;
- Skia rendering spike on all three operating systems;
- immutable audio-graph spike;
- plugin process-isolation feasibility spike;
- benchmark results on declared reference machines;
- dependency and licensing inventory;
- architecture decisions and risk register;
- Phase 1 backlog with estimates and owners.

## Week 1: Product and repository foundation

- Review the product brief with at least three composers and three sound
  designers.
- Rank the top ten workflow problems.
- Lock naming conventions, source layout, formatting, and CI.
- Create low-fidelity primary workspace prototypes.
- Record dependency candidates and licenses.

Exit evidence:

- interview notes and ranked problems;
- approved v1 scope;
- green skeleton build on three CI operating systems.

## Week 2: Platform probes

- Enumerate audio and MIDI devices.
- Open and close a minimal audio stream.
- Measure callback timing at 48 kHz with 64, 128, and 256 sample buffers.
- ASIO SDK licensing path resolved: proprietary selected; developer-agreement
  signing remains pending and does not block continued technical spikes.
- Prototype AWT/Skiko window, input, IME, accessibility, and native handles.
- Verify HiDPI and multi-monitor coordinate conversion.
- Follow the cross-platform callback measurement and soak sequence in
  [`AUDIO_PROBE_RUNBOOK.md`](AUDIO_PROBE_RUNBOOK.md).

Exit evidence:

- device probe logs from all three OSes;
- two-hour callback soak-test results;
- documented platform gaps.

## Week 3: Skiko/Skia raster and text spike

- Pin Skiko and its transitive runtime dependencies.
- Resolve reproducible desktop runtime artifacts per operating system.
- Render reference controls, waveform geometry, text, and automation curves.
- Add screenshot comparison.
- Test scaling at 100%, 125%, 150%, and 200%.

Exit evidence:

- matching reference screenshots;
- documented text shaping and font fallback behavior;
- raster performance baseline.

## Week 4: Skiko/Skia GPU spike

- Exercise the Skiko hardware-accelerated surface on every operating system.
- Record the actual Metal, Windows, and Linux rendering backends selected.
- Simulate resize, monitor movement, sleep/wake, and device loss.
- Measure dense arrangement rendering at 60 and 120 Hz.

Exit evidence:

- frame-time traces;
- renderer recovery test;
- accepted backend matrix or a documented fallback.

## Week 5: Real-time graph

- Implement track, gain, mix, and output nodes.
- Compile an immutable render plan.
- Add bounded command and telemetry queues.
- Replace routing while audio runs.
- Instrument allocations, locks, deadline misses, and denormals.

Exit evidence:

- automated graph correctness tests;
- zero callback allocations and locks;
- latency and CPU traces.

## Week 6: Disk and session resilience

- Stream audio through read-ahead buffers.
- Record to a recoverable temporary file.
- Prototype stable session identifiers and command journal.
- Simulate process termination during save and recording.

Exit evidence:

- recoverable recording after forced termination;
- atomic project save demonstration;
- project round-trip test.

## Week 7: Plugin isolation

- Scan a minimal CLAP and VST3 set out of process.
- Prototype shared-memory audio/control transport.
- Terminate the plugin process during playback.
- Identify native editor embedding constraints on each OS.

Exit evidence:

- main process survives plugin termination;
- measured bridge overhead;
- plugin UI risk report.

## Week 8: Review and Phase 1 commitment

- Run all benchmarks on reference hardware.
- Close or explicitly accept every critical risk.
- Review research findings with target users.
- Convert validated spikes into a Phase 1 backlog.
- Decide whether architecture ADR-0003 is accepted.

## Current task board

| ID | Task | Status |
|---|---|---|
| P0-001 | Product brief | Draft complete |
| P0-002 | v1 scope contract | Draft complete |
| P0-003 | Repository and CMake skeleton | Complete; Windows build verified |
| P0-004 | Three-OS skeleton CI | Complete; Windows/macOS/Linux build and test matrix green |
| P0-005 | Java/Skiko renderer spike | Windows reference window runs |
| P0-006 | UI toolchain bootstrap | Pinned bootstrap verified on Windows |
| P0-007 | Java–C++ process boundary | Complete for Phase 0 stdio transport; persistent handshake/load smoke green on three-OS CI |
| P0-008 | Audio callback probes | Windows open: 64 cadence failed; Core Audio/JACK probes added but target runs pending; proprietary SDK path resolved, signing pending; three 2h soaks pending |
| P0-009 | User interviews | Not started |
| P0-010 | Dependency/license inventory | Not started |
| P0-011 | Immutable real-time graph core | Windows production path, 5,001-publication sanitizer edit-storm, cross-block ramps, sample-rate modulation, and denormal protection verified; other backends and final soak pending |
| P0-012 | Disk and session resilience | Native session ownership, write-ahead journal, monotonic undo/redo, replay, immutable/coalesced saves, fixed-window autosave, shutdown flush, and revision-gated history compaction verified locally and on three-OS CI/Java/sanitizers; backup rotation, complete state, cold hardware benchmarks, and media/plugin restoration pending |
| P0-013 | Plugin process isolation | Not started |
| P0-014 | Phase 0 exit review and Phase 1 backlog | Not started |

## Definition of done

Phase 0 is done only when every mandatory spike has measured evidence on all
three target operating systems. “It compiles” is not sufficient evidence for
audio or rendering decisions.
