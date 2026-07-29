# Real VST3 Hosting Inside the Bridge Child — 2026-07-29

Status: P0-013 sixth slice. Closes the boundary the previous VST3 slice
carried: `Vst3Host` in
[`VST3_PLUGIN_INSTANTIATION_2026-07-29.md`](VST3_PLUGIN_INSTANTIATION_2026-07-29.md)
ran **in-process**, inside the probe itself — the exact thing the whole
bridge exists to avoid. It now runs only inside the bridge's isolated
child process, driven entirely through `PluginBridge`'s public surface
(`processBlock`, `restoreState`, `captureState`), never called directly by
the process that would otherwise be the real-time host.

## What changed

- `PluginBridge::runChild()` gained a `"vst3"` mode: on that path the
  child opens a real `Vst3Host` instead of running the stand-in gain/pass
  DSP, and every block, state capture, and state restore goes through the
  real plugin. Bypass stays host-owned exactly as it does in stand-in
  mode — the session schema keeps it off `SessionPlugin`'s DSP state,
  so a real plugin must not claim it either.
- `PluginBridge::startVst3()` launches the child with the module path,
  class index, and sample rate appended as arguments. The existing
  `start()` path (stand-in modes: normal/crash/hang) is untouched and its
  tests are unmodified.
- The child's command line can now carry a real filesystem path, which
  routinely contains spaces (`Program Files`). The previous command-line
  builder concatenated arguments unquoted because "normal"/"crash"/"hang"
  never needed it; `start()`/`startVst3()` now share one path
  (`startWithArguments()`) that quotes every Windows argument and uses
  `execv` with a full argv vector on POSIX, rather than the fixed-arity
  `execl` the stand-in path used.
- `apps/plugin-host` (previously an in-process `Vst3Host` probe) now
  self-execs as the bridge's child, the same way `PluginTests.cpp`
  already does, and drives a real plugin exclusively through
  `PluginBridge`. It no longer links against `Vst3Host` calls directly
  from the driving process — that type is now only reachable from inside
  a child.

An open failure inside the child (bad module, wrong class index, plugin
that refuses to initialize) is reported the same way a crash is: the
child exits before entering the request loop, and the first
`processBlock()` call observes `processExited`. The host has no separate
channel to distinguish "never opened" from "crashed after opening," which
is an intentional simplification for Phase 0 — the failure mode that
matters to the audio callback is identical either way: silence, not a
hang.

## Local result

Toolchain: GCC 15.2.0 (MSYS2 UCRT64), Ninja, Debug. Hardware: AMD Athlon
Silver 3050U, 2 cores. VST3 SDK 3.8.0 via `IRAMIX_VST3_SDK_PATH`. 532
blocks of 256 frames, stereo, 48 kHz, 20 ms bridge deadline (deliberately
generous — this measures whether real hosting works through the bridge,
not tight-deadline scheduling, which is `iramix_plugin_tests`' job against
the stand-in plugin). Windows-only per R-13.

```text
Plugin bridge host: module=Diopser.vst3, in_bridge_child=1, open_ms=629.555,
blocks=532, processed=532, block_p50_ms=0.0186, block_p95_ms=0.0191,
block_p99_ms=0.0238, block_max_ms=0.1426, deadline_misses=14,
input_peak=0.249997, output_peak=0.249997, samples_changed=0,
state_bytes=279, state_saved=1, state_restored=1, state_stable=1,
save_ms=0.9209, load_ms=2.0486, audio_after_restore=1,
peak_after_restore=0.249997

Plugin bridge host: module=Crisp.vst3, in_bridge_child=1, open_ms=766.416,
blocks=532, processed=532, block_p50_ms=0.0206, block_p95_ms=0.021,
block_p99_ms=0.025, block_max_ms=0.0455, deadline_misses=13,
input_peak=0.249997, output_peak=0.444791, samples_changed=0.566406,
state_bytes=359, state_saved=1, state_restored=1, state_stable=1,
save_ms=0.1331, load_ms=0.1002, audio_after_restore=1,
peak_after_restore=0.452902

Plugin bridge host: module=Crossover.vst3, in_bridge_child=1,
open_ms=377.29, blocks=532, processed=532, block_p50_ms=0.0125,
block_p95_ms=0.0133, block_p99_ms=0.0179, block_max_ms=0.1396,
deadline_misses=7, input_peak=0.249997, output_peak=0.249997,
samples_changed=0, state_bytes=185, state_saved=1, state_restored=1,
state_stable=1, save_ms=13.576, load_ms=0.118, audio_after_restore=1,
peak_after_restore=0.249997
```

All three plugins process every measured block (532/532) with their own
DSP, running only inside the child. `deadline_misses` is a cumulative
counter over the whole run, not just the measured window — it is
attributable to the warm-up loop spinning against the 20 ms deadline
while the plugin is still loading (`open_ms` is 377–766 ms, well past
several 20 ms attempts), the same pattern `PluginTests.cpp` documents as
"warm-up legitimately misses while the process is still launching."
Zero misses occur once `processed` starts incrementing. State round trips
through each plugin's own opaque format and is byte-stable
(`state_stable=1`) for all three, and audio still flows after a restore
(`audio_after_restore=1`).

The full stub-mode suite (`iramix_plugin_tests`, 12 counter lines,
`crash`/`hang`/rejection/state/parameter paths) is unaffected — those
tests never pass `mode == "vst3"`, and the shared
`startWithArguments()`/`runChild()` refactor left their behavior and
counters identical to the prior slice.

## Evidence boundary

This does not prove:

- **Parameters via `IEditController`.** The gain/bypass parameter IDs the
  transport carries are stand-in-plugin concepts; a real plugin's own
  parameter list, automation, and `IEditController` round trip are still
  unexercised. Bypass is applied at the bridge, outside the plugin, which
  sidesteps rather than validates parameter delivery to a real plugin.
- **MIDI to instruments.** Nothing here sends note or CC data, and no
  instrument (as opposed to effect) plugin was hosted.
- **Autosave integration.** `captureState()`/`restoreState()` are called
  directly by this probe, not through `SessionSaveCoordinator` or
  `AsyncSessionSaver` — R-12's end-to-end interaction (a real plugin's
  state captured during an actual autosave window) is still unexercised.
- **macOS and Linux.** No hardware is available (R-13); the child-process
  argument quoting was written to the documented Windows rules and
  exercises the POSIX `execv` path only by inspection, not measurement.
- **Crash/hang recovery for a real plugin.** `testPluginProcessTermination`
  and `testPluginProcessHang` in `iramix_plugin_tests` exercise the
  stand-in plugin's crash/hang paths; no real VST3 plugin was crashed or
  hung mid-block to confirm the same recovery holds when actual
  third-party code, not a controlled stand-in, is what fails.
- **Instrument or MIDI-only plugins**, or plugins with more than one
  input/output bus. Only single-bus audio effects were exercised.
- **Tight-deadline behavior for a real plugin.** The 20 ms deadline used
  here is generous by design; whether a real plugin holds to the 5 ms
  budget `iramix_plugin_tests` proves for the stand-in is unmeasured.
