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

- Pin Skiko and its transitive runtime dependencies — done (P0-006).
- Resolve reproducible desktop runtime artifacts per operating system.
- Render reference controls, waveform geometry, text, and automation
  curves — done
  ([`results/SKIA_RASTER_BASELINE_2026-07-29.md`](results/SKIA_RASTER_BASELINE_2026-07-29.md)).
- Add screenshot comparison — done, and verified by breaking it: a
  one-least-significant-bit colour change fails the run at all four
  scales.
- Test scaling at 100%, 125%, 150%, and 200% — done as canvas scale.
  This is *not* the same as running on a HiDPI display; no AWT/Skiko
  surface or `GraphicsConfiguration` transform is involved, so the Week 2
  HiDPI and multi-monitor item stays open.

Exit evidence:

- matching reference screenshots — done on all three operating systems.
  CI showed the four digests are **bit-identical** on `windows-x64`,
  `macos-arm64`, and `linux-x64`, across x86-64 and aarch64 alike, so
  the same baselines are committed for all three and the comparison now
  guards every CI leg. `macos-x64` and `linux-arm64` have never run and
  report `baseline=absent`;
- documented text shaping and font fallback behavior — done, with a
  finding that strengthened when it ran cross-platform: `TextLine.make`
  shapes with a single font and does **not** consult the font manager.
  Coverage is not portable — Arabic renders on Windows and Linux but is
  9/10 `.notdef` on macOS's Helvetica Neue, ASCII advance widths differ
  by up to 15% between platforms, and the Ubuntu runner has no Japanese
  face to fall back to at all. Iramix must drive fallback itself and
  must not hard-code measured text extents;
- raster performance baseline — done for CPU raster only. Full-scene
  repaint costs 6.0–12.0 ms p50 across the four scales, which misses a
  120 Hz budget from 150% upward on the reference machine.

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

- Scan a minimal CLAP and VST3 set out of process — done, and against 201
  real installed plugins rather than a minimal set
  ([`results/PLUGIN_SCAN_OUT_OF_PROCESS_2026-07-29.md`](results/PLUGIN_SCAN_OUT_OF_PROCESS_2026-07-29.md)).
  CLAP metadata is not decoded: the headers that would pin the struct
  layout are not available to this project.
- Prototype shared-memory audio/control transport.
- Terminate the plugin process during playback.
- Identify native editor embedding constraints on each OS.
- Restore persisted plugin state into a hosted plugin instance.

Exit evidence:

- main process survives plugin termination — done;
- measured bridge overhead — done;
- plugin UI risk report — done, as analysis only
  ([`results/PLUGIN_EDITOR_EMBEDDING_RISK_2026-07-29.md`](results/PLUGIN_EDITOR_EMBEDDING_RISK_2026-07-29.md));
- a session's stored plugin state blob reloaded into a live plugin — done.

Plugin state restoration moved here from P0-012. The schema v4 record
(format, restorable identifier, slot, bypass, opaque bounded state blob)
is stored and round-trips intact, but consuming it means starting a plugin
host, which is exactly this task's boundary. Implementing it under P0-012
would only have produced another layer that nothing consumes.

The restoration exit item now has evidence in
[`results/PLUGIN_STATE_RESTORATION_2026-07-28.md`](results/PLUGIN_STATE_RESTORATION_2026-07-28.md):
a blob authored into a session, serialized, decoded, and restored into the
live hosted plugin, with the restored coefficient observable in rendered
audio and the plugin's state captured back byte for byte. The plugin is a
stand-in, not a real CLAP or VST3.

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
| P0-005 | Java/Skiko renderer spike | Windows reference window runs. Raster slice done: a deterministic dense-arrangement scene (controls, waveform geometry, automation curves) renders byte-identically across processes, is compared against committed per-target PNG baselines at 100/125/150/200% in `gradle check` on all three CI OSes, and the comparison is verified by a one-LSB perturbation that fails all four scales. Raster output is measured bit-identical on Windows x86-64, macOS arm64, and Linux x86-64, so all three carry baselines and are guarded. CPU raster full repaint measured at 6.0–12.0 ms p50, which misses 120 Hz from 150% upward. Text shaping and font fallback documented rather than baselined: `TextLine.make` does not consult the font manager, so CJK and emoji shape entirely to `.notdef`. Frame-time tail is unattributed — GC counters rule the JVM heap out. GPU backend, windowing, presentation, device loss, and real HiDPI surfaces all untouched (R-03) |
| P0-006 | UI toolchain bootstrap | Pinned bootstrap verified on Windows |
| P0-007 | Java–C++ process boundary | Complete for Phase 0 stdio transport; persistent handshake/load smoke green on three-OS CI |
| P0-008 | Audio callback probes | Windows open: 64 cadence failed; Core Audio/JACK probes added but target runs pending; proprietary SDK path resolved, signing pending; three 2h soaks pending; shared-mode path first exercised via session device restoration, which fixed an exclusive-mode-shaped buffer assertion and a deadline target table that returned zero outside 64/128/256 (see DEVICE_ENUMERATION_WINDOWS_2026-07-28) |
| P0-009 | User interviews | Not started |
| P0-010 | Dependency/license inventory | Inventory separates in-build from candidates, Java licenses read from cached POMs and regenerable via `scripts/license-inventory.ps1`; C++ engine confirmed dependency-free. Four obligations open: Skiko native attribution (L-1), unverified JACK license (L-2), mutable CI action tags (L-3), annotations version skew (L-4); vulnerability inventory still unautomated |
| P0-011 | Immutable real-time graph core | Windows production path, 5,001-publication sanitizer edit-storm, cross-block ramps, sample-rate modulation, and denormal protection verified; other backends and final soak pending |
| P0-012 | Disk and session resilience | Native session ownership, write-ahead journal, monotonic undo/redo, replay, immutable/coalesced saves, fixed-window autosave, shutdown flush, revision-gated history compaction, revisioned backup rotation/retention, fail-closed automatic restore, and durable journal baselines verified locally and on three-OS CI/Java/sanitizers; and schema v4 media references, MIDI, device configuration, and plugin state verified locally and on three-OS CI/Java/sanitizers; media relink/restoration verified locally and on three-OS CI/sanitizers; device-configuration restoration decided and verified locally and on three-OS CI/sanitizers, with real WASAPI enumeration selecting a session's stored endpoint on Windows hardware and the audio callback measured on it at zero target/deadline misses (macOS/Linux enumeration absent); plugin state restoration moved to P0-013; macOS/Linux reference-hardware benchmarks accepted as out of scope for Phase 0 (R-13) |
| P0-013 | Plugin process isolation | Bridge slice done: shared-memory transport, bounded deadline degrading to silence, host survives child crash and hang, bridge overhead p99 19.1 us, orphan watchdog verified. State slice done: a session's stored blob restores into the live plugin and is observable in rendered audio, capture round trips byte for byte through persistence, six refusal paths verified, and a capture from a crashed plugin times out at its deadline instead of stalling a save. No plugin SDK involved. Control transport slice done: bounded lock-free SPSC parameter ring, events applied at their scheduled block and never early, saturation/lateness/reordering counted rather than dropped, and a transport change observable in both rendered audio and captured state. No plugin SDK involved. Editor embedding delivered as a risk report only — a live Windows probe was attempted and abandoned without isolating the cause, and macOS/Linux cannot be checked at all (R-13, R-04). Out-of-process scan done against 201 real installed plugins: 191 scanned, 188 named through VST3 SDK 3.8.0, 7 load failures and 3 load hangs recorded rather than fatal, zero host failures, and a Windows error-mode trap fixed that had made a bad module indistinguishable from a hang. CLAP modules load but are not decoded (no headers). Real VST3 hosting done in-process: five third-party plugins instantiated, 532/532 blocks processed each, state saved and restored through the plugin's own format, with a plugin found writing to stdout (which the stdio Java/C++ transport could not have survived) and one whose state is not byte-stable across a round trip. Real VST3 hosting moved into the bridge child — done: `PluginBridge::runChild()` now hosts a real `Vst3Host` on its `"vst3"` mode, driven only through `processBlock`/`restoreState`/`captureState`, closing the in-process gap the previous slice carried; three third-party plugins verified through the bridge, 532/532 blocks each, state round trips byte-stable, audio still flows after restore (see [`results/VST3_HOSTING_IN_BRIDGE_CHILD_2026-07-29.md`](results/VST3_HOSTING_IN_BRIDGE_CHILD_2026-07-29.md)). Plugin state capture wired into autosave — done: `captureLivePluginState()` refreshes a document's plugin state from a live bridge and hands it to a real `AutosaveClock`-scheduled `SessionPersistenceService` window, read back through `JournaledSession::open()` to confirm the durably saved file holds the live-captured state rather than a stale placeholder (see [`results/PLUGIN_STATE_AUTOSAVE_WIRING_2026-07-29.md`](results/PLUGIN_STATE_AUTOSAVE_WIRING_2026-07-29.md)); exercised against the stand-in plugin only, not yet joined to real VST3 hosting, and `SessionController`/`JournaledSession` still have no plugin-editing API for a real caller to invoke it from. Crash/hang recovery against a real plugin — done: `runChild()`'s `"vst3-crash"`/`"vst3-hang"` modes inject the same fault the stand-in uses, but only after three genuine blocks through the real plugin's own DSP; two third-party plugins verified for both faults, every degraded block silent, host process alive and responsive afterward in all four runs (see [`results/VST3_CRASH_HANG_RECOVERY_2026-07-29.md`](results/VST3_CRASH_HANG_RECOVERY_2026-07-29.md)). Real parameters via IEditController — done: `Vst3Host` acquires a real `IEditController` (same-object or separate-class), enumerates the plugin's own parameter list, and delivers changes through a minimal `IParameterChanges`/`IParamValueQueue` pair on `process()`, exactly where a real host delivers automation. The bridge's existing generic parameter transport now routes non-bypass IDs to it in `"vst3"` mode; three third-party plugins verified (6–11 real parameters each, plugin-assigned IDs), each change accepted and observably reaching the plugin's own `getState()` output (see [`results/VST3_IEDITCONTROLLER_PARAMETERS_2026-07-29.md`](results/VST3_IEDITCONTROLLER_PARAMETERS_2026-07-29.md)). MIDI to a real instrument — done: `Vst3Host` delivers notes through a minimal `IEventList`, the same pattern as parameter automation; the bridge gained a second, independent bounded lock-free SPSC ring so a note burst and an automation burst saturate independently. Verified against a real instrument (Vital, `in_channels=0`): measured silence before a note, substantial measured output after, nothing else changed (see [`results/VST3_MIDI_TO_INSTRUMENTS_2026-07-29.md`](results/VST3_MIDI_TO_INSTRUMENTS_2026-07-29.md)). This closes every item P0-013 listed as still pending; what remains unexercised is cataloged per-slice in each result document's evidence boundary, chiefly macOS/Linux hardware (R-13, accepted) and a session-editing caller for the plugin transports, which was never P0-013's scope |
| P0-014 | Phase 0 exit review and Phase 1 backlog | Not started |

## Definition of done

Phase 0 is done only when every mandatory spike has measured evidence on all
three target operating systems. “It compiles” is not sufficient evidence for
audio or rendering decisions.
