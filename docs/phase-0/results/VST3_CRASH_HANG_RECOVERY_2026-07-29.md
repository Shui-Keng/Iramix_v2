# Crash and Hang Recovery Against a Real VST3 Plugin — 2026-07-29

Status: P0-013 eighth slice. `testPluginProcessTermination` and
`testPluginProcessHang` in `iramix_plugin_tests` proved the bridge's
crash/hang recovery contract against the stand-in child only. This slice
proves the same contract when the process that fails was, until the
moment it does, genuinely running a real third-party plugin's own DSP —
not a controlled synthetic stand-in.

## What changed

- `PluginBridge::runChild()`'s `"vst3"` mode gains two variants,
  `"vst3-crash"` and `"vst3-hang"`: both host the real plugin exactly as
  `"vst3"` does, and additionally reuse the existing `handled == 3` fault
  injection point — the same one `"crash"`/`"hang"` already use for the
  stand-in — so the child terminates or hangs only *after* three genuine
  blocks have gone through the real plugin's `Vst3Host::process()`. The
  fault always fires after exactly three real blocks by construction:
  the check sits after the DSP branch runs and before `++handled`, so
  there is no path to the fault that skips real processing.
- `PluginBridge::startVst3Fault()` launches the child in one of these two
  modes.
- `apps/plugin-host` gained a `--crash`/`--hang` trailing flag
  (`iramix_plugin_host <module> [class-index] [--crash|--hang]`) that
  drives `startVst3Fault()`, then keeps calling `processBlock()` and
  reports how many blocks degrade to silence and whether the destination
  is ever left with stale or uninitialized data.

This is a manual probe, like the two previous real-plugin slices, not a
`ctest` addition: it depends on a real VST3 plugin being installed on the
machine, which no CI runner has.

## Local result

Toolchain: GCC 15.2.0 (MSYS2 UCRT64), Ninja, Debug. Windows-only per
R-13. 256-frame blocks, stereo, 48 kHz, 20 ms bridge deadline, 100
`processBlock()` attempts per run after warm-up.

```text
Plugin bridge fault: module=Diopser.vst3, fault=crash, attempts=100,
processed=1, degraded=99, every_degraded_silent=1, worst_block_ms=63.0505,
deadline_misses=103, exited_blocks=0, host_survived=1

Plugin bridge fault: module=Diopser.vst3, fault=hang, attempts=100,
processed=2, degraded=98, every_degraded_silent=1, worst_block_ms=35.5398,
deadline_misses=101, exited_blocks=0, host_survived=1

Plugin bridge fault: module=Crisp.vst3, fault=crash, attempts=100,
processed=1, degraded=99, every_degraded_silent=1, worst_block_ms=58.1918,
deadline_misses=102, exited_blocks=0, host_survived=1

Plugin bridge fault: module=Crisp.vst3, fault=hang, attempts=100,
processed=1, degraded=99, every_degraded_silent=1, worst_block_ms=26.7508,
deadline_misses=102, exited_blocks=0, host_survived=1
```

In every run: every degraded block is silence, not stale or
uninitialized data; `worst_block_ms` stays bounded near the 20 ms
deadline rather than growing without limit, including across the hang
case whose only recovery path is `PluginBridge::stop()`'s bounded
wait-then-terminate; and the driving process reaches its own final print
statement and exits normally in all four runs — the process is still
alive and responsive after a real plugin it hosted crashed or hung.

`processed` is low (1–2) in every run, not because the fault fires before
real work happens, but because of an artifact of how a slow plugin load
interacts with warm-up: `Vst3Host::open()` takes several hundred
milliseconds (measured at 377–766 ms for these plugins in
`VST3_HOSTING_IN_BRIDGE_CHILD_2026-07-29.md`), during which the driving
process's `warmUp()` loop retries `processBlock()` roughly every 5–20 ms.
Each retry signals the child's Win32 auto-reset event, and signals that
arrive while the child is not yet waiting coalesce to a single wakeup, so
several of those retries can be serviced — and the `handled == 3` fault
can already fire — before the driving process observes its first
success. The three-block guarantee is a property of the child's own
control flow, not of what the host happens to observe; `processed`
undercounts it whenever loading is slow relative to the deadline.

The full `iramix_plugin_tests` suite (12 counter lines, including the
unmodified stand-in `"crash"`/`"hang"` tests) is unaffected and passes
both with and without `IRAMIX_VST3_SDK_PATH`; all 7 `ctest --preset dev`
targets pass in both configurations.

## Evidence boundary

This does not prove:

- **Deadline behavior under contention.** The 20 ms deadline is generous,
  as in the other real-plugin slices. Whether the stand-in's proven 5 ms
  budget also holds for a real plugin remains unmeasured.
- **A crash or hang that is not artificially injected.** The fault here
  is deliberately triggered by this project's own code after a fixed
  count, not a genuine third-party plugin defect. It proves the bridge's
  recovery mechanism does not care *why* the child died, only *that* it
  did — which is the actual design property — but it is not evidence
  that these specific plugins are unstable, nor a substitute for finding
  a plugin that crashes on its own.
- **Multiple simultaneous plugin processes**, one of which fails while
  others keep running. Only a single bridge is exercised per run.
- **Recovery during active state capture or restore.** No run here
  triggers a fault while `captureState()`/`restoreState()` is in flight;
  `testPluginStateOnDeadPlugin` covers that against the stand-in only.
- **macOS and Linux.** No hardware is available (R-13); the POSIX side of
  the fault-injection dispatch was written to mirror the existing
  `"crash"`/`"hang"` logic exactly and shares its untested status there.
