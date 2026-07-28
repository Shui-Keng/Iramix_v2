# Plugin Parameter Transport — 2026-07-28

Status: P0-013 third slice. Completes the *control* half of the Week 7
item "prototype shared-memory audio/control transport" — only audio had
been crossing the boundary until now. Plugin scanning and editor embedding
remain open, and scanning is blocked on a licence decision rather than on
engineering. Week 7 remains open.

## Scope

The bridge could send a plugin audio and hand it a state blob. It had no
way to tell a running plugin that a parameter changed, which means nothing
a user does to a plugin between saves could reach it.

This slice adds a bounded lock-free SPSC ring in the same shared mapping:

- the host's **control thread** is the only writer, the plugin process the
  only reader, and the **audio thread touches neither index**;
- every event carries an **absolute sample timestamp**, and the plugin
  drains only up to the end of the block it is about to render;
- saturation, out-of-order timestamps, and late events are **counted
  outcomes**, never silent drops.

That shape is not new to this project — it is the same contract the engine
already uses for control→audio traffic, restated across a process
boundary.

## Why timestamps and not "apply on arrival"

A plugin that applied changes whenever they happened to arrive would
render differently depending on scheduling, so the same session would not
reproduce. Events are therefore refused if their timestamp goes backwards,
and an event scheduled for a later block **stays queued** rather than being
applied early.

The test asserts on rendered audio at three points, with an event
deliberately scheduled for the block *after* next so that "applied at the
right time" and "applied at all" cannot be confused:

1. the block before the event's timestamp still renders at the old
   coefficient — the event was **not** applied early;
2. the block the event was scheduled for renders at the new one;
3. a second, distinct parameter (bypass) produces distinct audible
   behaviour, so one stuck coefficient cannot satisfy both assertions.

## The loop this closes

Parameter changes feed the same live state that `captureState()` reads, so
a value set through the transport is what a subsequent save would persist.
The test sets gain to 0.2 through the queue, captures, and decodes 0.2 out
of the blob.

Combined with the previous slice, the full round trip now has evidence:

**session load → restore → user changes a parameter → capture → save.**

Bypass is deliberately excluded from the blob. `SessionPlugin` keeps
`bypassed` as a host-side field, so the plugin must not claim ownership of
it; the test asserts the captured blob is byte-identical to one encoding
gain alone.

## Local Release result

Toolchain: GCC 15.2.0 (MSYS2 UCRT64), Ninja, `CMAKE_BUILD_TYPE=Release`.
Hardware: AMD Athlon Silver 3050U, 2 cores / 2 logical processors.
Windows-only per accepted risk R-13.

```text
Plugin parameter transport: queue_capacity=64, sent=2, applied=2, late=0,
early_application=0, bypass_observed=1, capture_reflects_transport=1

Plugin parameter limits: late_applied=1, late_dropped=0, queue_capacity=64,
accepted=64, overflows=8, out_of_order=1, after_stop=1
```

`accepted=64, overflows=8` is the saturation case: with no block being
processed the plugin never drains, so the queue accepts exactly its
capacity and refuses the next eight at the call site. Nothing is
overwritten — a bounded queue that discarded its oldest entry would lose an
automation move with no trace of having done so.

`late_applied=1, late_dropped=0` is the opposite failure. An event whose
timestamp has already passed is still applied, and counted as late. Late
delivery is a scheduling failure upstream; discarding the event would turn
it into a silently wrong render.

No timing figures are quoted for the enqueue. It is a bounded store into a
mapped ring with no syscall, and measuring it on this machine would produce
a number that says more about the timer than about the transport.

## Refusal paths verified

| Case | Result |
|---|---|
| Queue full | `queueFull`, counted, nothing overwritten |
| Timestamp earlier than one already queued | `outOfOrder`, counted |
| Bridge configured without a parameter queue | `unavailable` |
| Before `start()` / after `stop()` | `unavailable` |

`parameterOverflows` and `parameterOutOfOrder` are separate counters: the
two refusals mean different things about the caller and should not be
summed.

## Evidence boundary

This proves the control transport contract — bounded queue, timestamped
application at the correct block, no early application, counted saturation
and lateness, refused reordering, and a parameter change observable both in
rendered audio and in captured state.

It does not prove:

- **anything about a real plugin's parameter model.** The stand-in exposes
  two parameters chosen because their effect is observable. Real plugins
  have hundreds, publish their own ranges and curves, may refuse values,
  and may demand sample-accurate application *within* a block. Nothing here
  is validated against CLAP or VST3 semantics;
- **sample-accurate automation.** Events are applied at block boundaries.
  An event landing mid-block is applied at that block's start, which is
  audible as a step at up to 256 frames of error. Splitting blocks at event
  boundaries is not implemented;
- **anything about parameter *output*.** Traffic is one-way. A plugin that
  changes its own parameters — from its editor, or from an internal LFO —
  has no way to tell the host, and the host would overwrite it at the next
  save;
- **behaviour under a realistic automation load.** Two events in the timing
  test and 72 in the saturation test. No sustained automation stream, no
  measurement of what queue depth a real session needs, and no figure for
  enqueue cost;
- **that the control thread is the only writer in practice.** The SPSC
  contract is asserted by construction and by TSan, not enforced. A second
  thread calling `setParameter()` concurrently would corrupt the ring;
- behaviour on macOS or Linux hardware (R-13), or under sanitizers beyond
  what CI reports;
- editor embedding or plugin scanning, both still untouched.
