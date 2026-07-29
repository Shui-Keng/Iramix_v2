# MIDI to a Real Instrument — 2026-07-29

Status: P0-013 tenth slice, closing the last "still pending" item on the
task board. Every previous P0-013 slice hosted audio *effects*
(Diopser, Crisp, Crossover — all take audio in, process it, and produce
audio out). This slice sends MIDI to a real *instrument* — a plugin with
no audio input bus at all, whose only output is what a note produces —
through the bridge's own transport, and proves it with the strongest
evidence this project's plugin work has used so far: silence, then sound,
with nothing else changed.

## What changed

- `Vst3Host` gained `sendNoteOn()`/`sendNoteOff()`, delivered the same way
  parameter changes are: built into a minimal `IEventList` (following the
  existing `IParameterChanges` pattern — interface header only, no SDK
  source compiled) and passed through `ProcessData::inputEvents` on the
  next `process()` call. Bounded to 32 pending events between blocks; a
  note is never coalesced the way a parameter value is, so a full queue
  refuses the new event rather than silently replacing an existing one.
- `PluginBridge` gained a second bounded lock-free SPSC ring, entirely
  separate from the parameter ring: `SharedMidiEvent` (sample time, type,
  channel, pitch, velocity), its own capacity/write/read indices, and its
  own `midiEventsApplied`/`midiEventsLate` counters. Separate rather than
  reusing the parameter ring because a note burst and an automation burst
  must saturate independently — one must not be able to starve the other.
  `PluginBridgeConfig::midiQueueCapacity` controls it the same way
  `parameterQueueCapacity` does; zero disables MIDI transport entirely,
  matching every other optional region in this design.
- `PluginBridge::sendMidiNoteOn()`/`sendMidiNoteOff()` enqueue with the
  same ordering guarantee as `setParameterById()`: a timestamp earlier
  than one already queued is refused, not reordered, because a rendered
  result that depends on delivery order rather than the timeline is not
  reproducible from a session.
- `runChild()`'s MIDI drain reuses the parameter drain's exact windowing
  discipline: events are applied only up to the current block's end, and
  an event scheduled for a later block waits rather than firing early.
  Delivery only does anything in `"vst3"`/`"vst3-crash"`/`"vst3-hang"`
  modes — the stand-in has no MIDI concept — but a stub-mode child with
  `midiQueueCapacity != 0` still advances its read index, so the
  bridge-level ring itself is exercisable independently of a real plugin.
- `apps/plugin-host` gained a `--midi` flag: it plays middle C on a real
  instrument through the bridge and reports peak output before and after
  the note.

## Local result

Toolchain: GCC 15.2.0 (MSYS2 UCRT64), Ninja, Debug. Windows-only per
R-13. Instrument: Vital (VST3), previously confirmed in
`VST3_PLUGIN_INSTANTIATION_2026-07-29.md` to have `in_channels=0` — an
instrument, not an effect, so its silence with no note playing is
guaranteed by the plugin's own architecture, not merely observed.

```text
Plugin bridge midi: module=Vital.vst3, pitch=60, note_on_status=accepted,
note_off_status=accepted, midi_events_sent=2, midi_events_applied=2,
peak_before_note=0, peak_after_note_on=0.358717, sound_from_silence=1
```

Reproduced identically on a second run. Five blocks measured before the
note: peak exactly `0`, not merely low — with no audio input bus, there
is nothing else that could have produced sound. Twenty blocks measured
after `sendMidiNoteOn()`: peak `0.358717`. `midi_events_applied` (the
child's own tally, not the host's) matches `midi_events_sent` exactly:
both the note-on and the note-off were drained and handed to the plugin.

This is a stronger proof than the parameter slice's state-blob diff
(`VST3_IEDITCONTROLLER_PARAMETERS_2026-07-29.md`): rather than inferring
that a change reached the plugin from a difference in its serialized
state, here a plugin that architecturally cannot make sound without a
note went from measured silence to measured, substantial output, with the
only difference between the two windows being the MIDI event sent through
the bridge.

The full `iramix_plugin_tests` suite (12 counter lines, unmodified) is
unaffected and passes both with and without `IRAMIX_VST3_SDK_PATH`; all 7
`ctest --preset dev` targets pass in both configurations.

## Evidence boundary

This does not prove:

- **Sustained polyphony or overlapping notes.** One note, played and
  released, is exercised. Nothing here plays a chord or two overlapping
  notes with independent release times.
- **MIDI CC, pitch bend, or aftertouch.** Only `NoteOnEvent`/`NoteOffEvent`
  are implemented; `Event`'s other union members
  (`PolyPressureEvent`, `ChordEvent`, etc.) are declared in the SDK header
  this project already includes but nothing here constructs one.
- **Multiple instruments in the same session**, or an instrument mixed
  with effect plugins downstream. Only one bridge, one plugin, is
  exercised.
- **A note's audible correctness** — pitch, timbre, envelope shape. Peak
  amplitude going from zero to non-zero is proof a note *reached and
  triggered* real synthesis; it says nothing about whether the rendered
  pitch is actually middle C or whether the envelope is well-formed.
- **The bridge-level MIDI ring exercised in `ctest`.** Unlike the
  stand-in's parameter transport (which has dedicated `ctest` coverage
  in `testParameterTransport`/`testParameterLateAndSaturation`), no
  automated test drives `midiQueueCapacity != 0` against the stand-in;
  the ring's saturation, ordering-refusal, and lateness paths inherit the
  parameter ring's design and code shape but are unverified by an
  automated assertion of their own.
- **macOS and Linux.** No hardware is available (R-13).

## P0-013 status

This closes the last item on the task board's "still pending" list for
P0-013 (`PLAN.md`): bridge-hosted real VST3 plugins, plugin state
wired into a real autosave window, crash/hang recovery against real
plugins, real parameters via `IEditController`, and now MIDI to a real
instrument are all evidenced. What remains unexercised across the whole
P0-013 arc — cataloged individually in each slice's own evidence
boundary — is macOS/Linux hardware (R-13, accepted), running the bridge
inside a real session-editing path (`SessionController` has no
plugin-editing API yet), and joining the parameter/MIDI transports to
session automation lanes, none of which were ever P0-013's stated scope.
