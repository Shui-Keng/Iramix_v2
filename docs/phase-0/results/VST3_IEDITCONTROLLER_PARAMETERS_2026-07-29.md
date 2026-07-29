# Real Parameters via IEditController — 2026-07-29

Status: P0-013 ninth slice, closing one of the two remaining "still
pending" items. Every previous P0-013 slice that touched parameters used
the stand-in plugin's synthetic gain/bypass concept. This slice reads a
real plugin's own parameter list through `IEditController` and delivers
changes to it through the bridge's existing generic parameter transport —
`PluginParameterId::gain` never applied to a real plugin at all, and does
not here either; only `bypass` stays meaningful across both.

## What changed

- `Vst3Host` acquires an `IEditController`, handling both shapes the SDK
  allows: most plugins implement it on the same object as `IComponent`
  (`queryInterface` succeeds directly); the SDK also permits a separate
  controller class, reached through `getControllerClassId()` and a second
  `factory->createInstance()`. The factory is now kept alive one call
  longer than before to make the second path possible; `close()` tears
  down a separate controller instance and skips a second release when it
  is the same object as the component.
- `Vst3Host::parameterInfo(index, info)` exposes the plugin's own
  parameter id, title, and default — via `getParameterInfo()`, never
  invented. `Vst3HostInfo::parameterCount` (declared since the first VST3
  slice, never populated until now) is filled in from
  `getParameterCount()`.
- `Vst3Host::setParameterNormalized(id, value)` is delivered the way a
  real host delivers automation: built into a minimal
  `IParameterChanges`/`IParamValueQueue` pair (following the project's
  existing `MemoryStream`/`HostApplication` pattern — interface headers
  only, no SDK source compiled) and passed through
  `ProcessData::inputParameterChanges` on the next `process()` call, not
  written directly into the plugin's state. Bounded to 32 pending
  parameters between blocks; a 33rd distinct parameter changed before the
  next block evicts the oldest pending one rather than allocating —
  `controller->setParamNormalized()` still has the authoritative value
  immediately, so only that one parameter's *delivery* to this block's
  audio is what would be lost, not the value itself.
- `PluginBridge::runChild()`'s `"vst3"`/`"vst3-crash"`/`"vst3-hang"` modes
  route every parameter event whose ID is not the reserved bypass ID to
  `setParameterNormalized()` instead of the stand-in's "gain" handling.
  Bypass stays host-owned exactly as before, regardless of plugin kind.
- New `PluginBridge::setParameterById()` — the same transport as
  `setParameter()`, addressed by a plugin's own raw ID rather than the
  stand-in's enum. New `PluginBridge::parameterMetadata()` reads three
  fields (`vst3ParameterCount`, `vst3FirstParameterId`,
  `vst3FirstParameterDefaultBits`) a `"vst3"` child writes once, before
  signaling ready — enough for a caller to address one real parameter by
  its own ID without a general per-parameter enumeration protocol across
  the process boundary.
- `apps/plugin-host` gained a `--params` flag: it reads the metadata,
  captures state, sets the first parameter to a value away from its
  default, drives ten blocks, captures state again, and reports whether
  the bytes differ. Manual probe, not a `ctest` addition, for the same
  reason as the other real-plugin slices.

## Local result

Toolchain: GCC 15.2.0 (MSYS2 UCRT64), Ninja, Debug. Windows-only per
R-13.

```text
Plugin bridge parameters: module=Diopser.vst3, parameter_count=8,
first_parameter_id=773352680, first_parameter_default=0, requested_value=1,
set_status=accepted, parameters_applied=1, state_before_bytes=279,
state_after_bytes=278, state_changed_by_parameter=1

Plugin bridge parameters: module=Crisp.vst3, parameter_count=11,
first_parameter_id=733630552, first_parameter_default=0.35,
requested_value=1, set_status=accepted, parameters_applied=1,
state_before_bytes=359, state_after_bytes=358, state_changed_by_parameter=1

Plugin bridge parameters: module=Crossover.vst3, parameter_count=6,
first_parameter_id=1810223892, first_parameter_default=0,
requested_value=1, set_status=accepted, parameters_applied=1,
state_before_bytes=185, state_after_bytes=185, state_changed_by_parameter=1
```

All three plugins expose a non-empty real parameter list (6–11
parameters) with plugin-assigned IDs in the billions range — not small
sequential indices, confirming these are the plugins' own identifiers,
not something this host invented. In every run the requested change was
accepted, exactly one parameter application was counted, and the
plugin's own `getState()` output changed as a result — proof the change
reached the plugin's real parameter handling, not merely the transport,
since the transport and the state serialization are entirely separate
code paths inside the plugin.

Correctness here is judged by the plugin's own state, not by an assumed
audible effect: an arbitrary first parameter is not guaranteed to be
audible against a fixed test tone, and may address something silent (a
mode switch, a bypass-like flag internal to the plugin). A state diff is
proof that real code executed on the change; it is not proof the change
was audible.

The full `iramix_plugin_tests` suite (12 counter lines, including the
stub-mode parameter transport and parameter-limits tests, unmodified) is
unaffected and passes both with and without `IRAMIX_VST3_SDK_PATH`; all 7
`ctest --preset dev` targets pass in both configurations.

## Evidence boundary

This does not prove:

- **Automation ramps.** Every change here lands at sample offset zero
  within a block — a single point, not a ramp across the block. VST3
  supports multi-point parameter automation within one block;
  `ParameterChangeQueue::addPoint()` is deliberately unimplemented
  (`kNotImplemented`) because nothing in this host builds anything but a
  single point.
- **The separate-controller-class path**, in practice. All three plugins
  tested implement `IEditController` on the same object as `IComponent`,
  so the `getControllerClassId()`/second-`createInstance()` fallback path
  is implemented and compiles but has not been exercised against a real
  plugin that actually uses it.
- **More than one parameter changed per test run**, or two parameters
  changed in the same block. Only the first enumerated parameter, one
  value, is exercised.
- **UI or automation-lane round trips.** Nothing here writes a parameter
  change into a session document or drives it from the arrangement
  timeline; the value is set directly against the live bridge and never
  touches `SessionDocument`/`SessionController`, matching the boundary
  the previous autosave-wiring slice already noted (neither has a
  plugin-editing API yet).
- **Non-ASCII parameter titles.** `parameterInfo()`'s title conversion is
  best-effort ASCII; a title with non-ASCII characters degrades those
  characters to `?` rather than being transcoded correctly. No plugin
  tested here exercises that path either way — it was not something this
  slice checked for.
- **macOS and Linux.** No hardware is available (R-13).
