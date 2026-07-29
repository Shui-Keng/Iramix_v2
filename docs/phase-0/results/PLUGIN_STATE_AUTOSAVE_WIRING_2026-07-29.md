# Plugin State Capture Wired Into Autosave — 2026-07-29

Status: P0-013 seventh slice, closing part of R-12's remaining gap.
`PLUGIN_STATE_RESTORATION_2026-07-28.md` proved `PluginBridge::captureState()`
hands a live plugin's state back correctly; it never touched autosave.
This slice drives the two together through a real, `AutosaveClock`-scheduled
autosave window and reads the result back through the project store, not a
shortcut past it.

## What changed

- New `iramix::plugin::captureLivePluginState()`
  (`include/iramix/plugin/PluginStateAutosave.hpp`,
  `src/plugin/PluginStateAutosave.cpp`): given a `SessionDocument` and a set
  of `LivePluginBinding {stableId, PluginBridge*}`, it refreshes each
  matching `SessionPlugin::state` from `PluginBridge::captureState()` and
  returns an `ImmutableSessionSnapshot` ready for
  `SessionPersistenceService::markDirty()` / `requestSave()`.
- A plugin with no live binding, or whose bridge does not answer `ok`
  within its `stateDeadline`, keeps the state already in the document
  rather than losing it — matching the project's existing "a refused or
  unavailable transfer leaves the previous state in force" rule for
  `restoreState()`. The `PluginStateCaptureReport` distinguishes
  "unchanged because there was no live binding" from "unchanged because
  the live plugin did not answer," so a caller (and this test) can tell
  the two apart rather than only seeing a single ambiguous count.
- `SessionController` and `JournaledSession` still have no plugin-editing
  API — that is a separate, larger feature this slice does not add. The
  new function operates directly on `SessionDocument`, the same layer
  `PluginTests.cpp`'s existing `makePluginSession()` helper already builds
  documents at, so it is usable today without that API existing yet.

## Local result

Toolchain: GCC 15.2.0 (MSYS2 UCRT64), Ninja, Debug. Windows-only per R-13.
New test `testPluginStateWiredIntoAutosave` in `tests/PluginTests.cpp`:

1. Builds a `SessionDocument` with one `SessionPlugin` holding a stale
   placeholder state blob.
2. Starts a real `PluginBridge` (stand-in child) and gives it a
   distinctive coefficient the placeholder cannot already hold.
3. Calls `captureLivePluginState()` to refresh the document from the live
   bridge and hands the result to a real `SessionPersistenceService`
   backed by a `ManualAutosaveClock`.
4. Advances virtual time across the autosave window and waits for the
   revision to commit.
5. Reopens the project through `JournaledSession::open()` — the same
   read path `testAutomaticBackupRestore` uses — and asserts the
   durably saved plugin state equals a direct `captureState()` read
   taken as a control, not the original placeholder.

```text
Plugin state autosave: captured=1, unchanged=0, failed=0,
autosaved_state_bytes=16, durable_revision=1, matches_live_capture=1
```

The full `iramix.plugin` ctest target (12 counter lines, including the
existing crash/hang/rejection/state/parameter suites) is unaffected and
passes both with and without `IRAMIX_VST3_SDK_PATH` set — this slice adds
no VST3-specific code. All 7 `ctest --preset dev` targets pass in both
configurations.

## Evidence boundary

This does not prove:

- **A real plugin captured through autosave.** The bridge here runs in
  stand-in mode (`"normal"`), not `"vst3"`. Nothing in this slice re-runs
  the real-plugin bridge hosting from
  `results/VST3_HOSTING_IN_BRIDGE_CHILD_2026-07-29.md` through the
  autosave path; the two closures are adjacent but not yet joined.
- **Multiple plugins, or a plugin that fails mid-capture.** The test
  exercises exactly one bound plugin taking the success path. The
  `unchanged`/`failed` distinction in `PluginStateCaptureReport` is
  implemented and typed but not exercised by an automated assertion here
  — no test currently drives a bridge into a state where `captureState()`
  times out or is rejected at the moment an autosave window fires.
- **A real session-editing path.** `captureLivePluginState()` is called
  directly against a hand-built `SessionDocument`, not through
  `SessionController`/`JournaledSession`, because neither has a
  plugin-editing API yet. Whoever eventually owns live `PluginBridge`
  instances during real editing (not yet designed) still needs to call
  this at the right moment relative to `markDirty()`; this slice supplies
  the mechanism, not that caller.
- **Concurrent autosave and audio.** The capture and the autosave window
  both run on the control thread in this test, sequentially. Nothing here
  measures what happens if an edit or another capture arrives while a
  capture from the previous window is still in flight.
- **macOS and Linux.** No hardware is available (R-13).
