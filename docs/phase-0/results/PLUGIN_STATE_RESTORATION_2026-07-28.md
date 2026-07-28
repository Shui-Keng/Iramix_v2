# Plugin State Restoration — 2026-07-28

Status: P0-013 second slice. Closes the chain a session's stored plugin
state has to travel — authored, serialized, decoded, restored into a live
hosted plugin, and captured back — plus the bounds that keep a plugin from
holding a save hostage. Plugin scanning and editor embedding are still
untouched. Week 7 remains open.

## Scope

Schema v4 has carried a plugin record since P0-012: format, restorable
identifier, slot, bypass, and an opaque bounded state blob. Nothing
consumed it. The record round-tripped through persistence and stopped
there, which is why the task was moved to P0-013 rather than closed under
P0-012 — consuming a state blob means starting a plugin host, and the host
did not exist until the bridge slice.

This slice adds the transfer path and the two bounds that matter:

- `restoreState()` hands a blob to the live plugin and reports what the
  plugin decided about it;
- `captureState()` reads the plugin's current state back for saving;
- both run on the **control thread**, never the audio callback, and both
  are bounded by a configured `stateDeadline`;
- the blob travels in its own region of the same shared mapping, on its own
  sequence pair, so the child applies it **strictly between blocks**. Audio
  is never rendered against a half-restored plugin.

It still deliberately carries **no plugin SDK**, for the same reason as the
bridge slice: CLAP and VST3 are unapproved candidates in `DEPENDENCIES.md`.

## Proving the blob reached the DSP

Copying bytes into another process proves nothing about restoration. A
transfer that silently discarded the state would still return success and
still leave the destination buffer full of plausible audio.

The stand-in plugin's state therefore encodes the coefficient its DSP
actually multiplies by, and the test asserts on **rendered audio** at three
points:

1. before any restore, output is `input × 0.5` — the stand-in's
   instantiation default, so a later change cannot be mistaken for a no-op;
2. after restoring a blob encoding `0.25`, output is `input × 0.25`;
3. after a *rejected* restore encoding `0.75`, output is still
   `input × 0.25` — a refused blob leaves the previous state in force
   rather than half-loading.

The blob also carries a 256-byte payload the host never interprets, as a
real plugin's state would, and `captureState()` returns the blob **byte for
byte**, payload included.

## Local Release result

Toolchain: GCC 15.2.0 (MSYS2 UCRT64), Ninja, `CMAKE_BUILD_TYPE=Release`.
Hardware: AMD Athlon Silver 3050U, 2 cores / 2 logical processors.
Windows-only per accepted risk R-13.

```text
Plugin state restoration: state_bytes=272, payload_bytes=256,
schema_version=4, restored_gain=0.25, restore_ms=0.0087,
capture_ms=0.0308, capture_identical=1, rejections=0, timeouts=0

Plugin state rejections: no_region=1, before_start=2, oversized=1,
corrupt=1, after_stop=1, previous_state_retained=1

Plugin state on dead plugin: state_deadline_ms=100, observed_ms=108.156,
timeouts=1, captured_bytes=0
```

The bridge counters from the first slice are unchanged in shape and still
pass on the same run:

```text
Plugin bridge healthy: blocks=500, frames=256, channels=2,
round_trip_p50_ms=0.0042, round_trip_p95_ms=0.0055,
round_trip_p99_ms=0.0158, round_trip_max_ms=1.0792, deadline_misses=0
```

`restore_ms` and `capture_ms` are single observations, not distributions.
They are reported to show the operation is in the tens of microseconds for
a 272-byte blob against an awake plugin — not as a percentile claim, and
not as a bound. The bound is `stateDeadline`, and the third line is what
demonstrates it.

## What the session contributes

The blob under test is not handed straight to the bridge. It is written
into a `SessionDocument`, serialized, deserialized, and taken from the
decoded document, so the bytes that reach the plugin are the bytes
persistence produced. The test asserts the decoded blob equals the authored
one before restoring it, and re-serializes the *captured* blob to confirm a
save built from live plugin state still loads.

This is the exit evidence Week 7 asks for: a session's stored plugin state
blob reloaded into a live plugin instance.

## Bounds against R-12

R-12 is "plugin state blocks autosave or recovery". Three bounds now have
evidence:

- **Size.** The region is sized once, before audio flows. A blob larger
  than the configured capacity is refused *before anything is written* into
  shared memory. The bridge's ceiling matches
  `kMaximumPluginStateBytes` in the session schema, so a blob persistence
  would refuse to store is not transferable either.
- **Time.** A capture from a plugin that has crashed returns `timedOut`
  after 108 ms against a 100 ms deadline, with no state, rather than
  blocking the caller. A save is delayed by one deadline; it is not held
  hostage.
- **Thread.** Neither operation runs on the audio callback. `awaitState()`
  sleeps between polls, which is legal precisely because it is control
  thread; `processBlock()` still spins and still allocates nothing.

## Refusal paths verified

| Case | Result |
|---|---|
| Bridge configured with no state region | `unavailable` |
| Restore or capture before `start()` | `unavailable` (both) |
| Blob larger than the configured capacity | `tooLarge`, nothing written |
| Blob failing its checksum | `rejectedByPlugin`, prior state intact |
| Restore after `stop()` | `unavailable` |
| Capture from a crashed plugin | `timedOut`, no state returned |

A rejected restore increments `stateRejections` and **not** `stateRestores`:
the counters distinguish "the plugin was asked" from "the plugin agreed".

## Evidence boundary

This proves the state transfer contract — bounded restore and capture off
the audio thread, byte-exact round trip through persistence and back, a
restored coefficient observable in rendered audio, previous state retained
on rejection, and no unbounded wait on a dead plugin.

It does not prove:

- **anything about a real plugin's state.** The blob format is the
  stand-in's own, documented and inspectable precisely so the audio
  assertion is possible. A real CLAP or VST3 blob is opaque, is produced by
  code this project does not control, and may be far larger, versioned, or
  refused for reasons the host cannot anticipate. No CLAP, VST3, or Audio
  Unit is scanned, loaded, or hosted here;
- **that restoration is correct across plugin versions or hosts.** The same
  process produced and consumed the blob, in the same run, from the same
  build. Nothing is said about a blob written by an older Iramix, a
  different machine, or a different plugin version;
- **any latency distribution for state transfer.** `restore_ms` and
  `capture_ms` are one sample each, against a 272-byte blob and an awake
  plugin. Behaviour at the 16 MiB ceiling, under memory pressure, or with
  several plugins capturing at once is unmeasured;
- **that autosave interacts correctly with capture.** The save coordinator
  does not call `captureState()` yet; nothing wires plugin state into the
  autosave window, so R-12's "blocks autosave" phrasing is bounded in
  principle here but not exercised end to end;
- behaviour on macOS or Linux hardware (R-13), or under sanitizers beyond
  what CI reports;
- editor embedding, control/parameter transport, plugin scanning, or bridge
  behaviour with more than one plugin process.
