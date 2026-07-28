# Plugin Editor Embedding — Risk Report — 2026-07-29

Status: P0-013. This is the Week 7 "plugin UI risk report" exit item. It is
**analysis, not measurement**. An attempt to measure the Windows case is
recorded below, including the fact that it did not succeed.

## Why this is the hardest remaining plugin problem

Every other part of plugin isolation moves *data* across a process
boundary: audio, a state blob, parameter events. All three are now measured
and all three were tractable because the bridge fully controls the format
on both sides.

An editor is different. It moves a **native window** across the boundary,
and the host does not control either end: the window is created by code
this project did not write, using a toolkit this project did not choose,
and it has to appear inside a surface owned by a *third* process.

That third process is the problem specific to this architecture.

## Three processes, not two

Iramix already runs a Java/Skiko UI process and a C++ engine process. The
plugin bridge adds a third. A plugin editor therefore has to be composed
across a boundary that most DAWs do not have:

| Owns | Process |
|---|---|
| The window the user sees | Java/Skiko UI process |
| The audio graph and the bridge | C++ engine process |
| The editor window itself | plugin host child process |

The plugin's window must be parented into a surface owned by the **Java**
process, while the process that knows when to open and close it is the
**engine**. The handle has to cross two boundaries, and the lifetime rules
of all three have to agree.

This interacts directly with R-07 (AWT/Skiko native-window behaviour): the
container is an AWT-derived surface, so whatever AWT does about DPI, focus,
and native handles becomes a constraint on every plugin editor.

## What was attempted, and what happened

A live Windows probe was built and then removed. The intent was to have the
plugin child process create its editor as a `WS_CHILD` of a window owned by
the host, verify the parenting, and confirm audio kept flowing.

It did not work, and the honest summary is that **the plugin process did
not end up displaying an embedded window**, with the cause not isolated
before the attempt was stopped. Two things were established on the way, and
both are real:

- **`GetParent` is not a valid check for embedding.** For a popup window it
  returns the *owner*, not the parent, so an early version of the probe
  reported success for a window that had never been embedded.
  `GetAncestor(hwnd, GA_PARENT)` is the correct query. A verification that
  uses the wrong one will pass while embedding is broken.
- **The plugin process needs a real UI thread, and its message loop is
  load-bearing.** Reparenting sends messages to the window's owning thread.
  A plugin whose UI thread is not pumping does not fail the call — it
  *blocks the caller*, so the host hangs in the act of showing an editor.
  This is a hang mode with no counterpart anywhere else in the bridge,
  where every wait is bounded by construction.

The probe was removed rather than left disabled. An API with no working
implementation and no passing test is the "layer nothing consumes" pattern
this project already rejected once, when plugin state restoration was moved
out of P0-012.

## Per-platform constraints

Unverified. Recorded so the Phase 1 estimate is not made blind.

### Windows

Cross-process `WS_CHILD` parenting is the conventional mechanism and is
what CLAP and VST3 hosts use. Known constraints: DPI awareness is per
window and a mismatch between host and plugin produces wrong scaling that
neither side can fix alone; input queues attach when windows are parented
across threads, which couples the two processes' responsiveness; and
process creation flags affect what a child may do with windows at all.

### macOS

`NSView` embedding across processes is not available the way it is on
Windows. The supported mechanism is a remote view service, which is a
different architecture rather than a different call — the plugin does not
hand over a view, it hosts one and the system composites it. Sandboxing and
window-server access apply. **No macOS hardware is available to this
project (R-13), so nothing here can be checked.**

### Linux

The highest-risk platform, and R-04 already carries it at priority 16. Under
X11 the mechanism is XEmbed and reparenting works. Under **native Wayland
there is no equivalent**: a client cannot reparent another client's surface,
and the available protocols are not a general substitute. Plugins that only
support X11 need XWayland, which then reintroduces scaling and input
differences. **No Linux hardware is available either.**

## Consequences for Phase 1

1. **The generic-editor fallback is not optional.** With three processes, no
   macOS or Linux hardware to validate on, and native Wayland lacking a
   mechanism entirely, a host-drawn generic editor built from the parameter
   list is the only editor path that can be guaranteed on every target. The
   parameter transport landed in this task is what makes that possible, and
   it is one-way today — a generic editor needs parameter *output* from the
   plugin, which does not exist yet.
2. **Editor embedding must not be estimated from the bridge's track record.**
   Audio, state, and parameters were all delivered against bounded,
   self-defined contracts. Editors are bounded by three platform APIs, one
   of which has no mechanism at all.
3. **R-04 cannot be closed without Linux hardware.** This is the same
   constraint as R-13 and belongs in the risk register as such, not as a
   perpetually pending task.

## Evidence boundary

This document measures **nothing**. It contains no counter line because no
figure was produced.

Specifically, it does not establish:

- that cross-process editor embedding works on Windows. It was attempted
  and abandoned;
- anything at all about macOS or Linux, where no hardware is available;
- anything about a real plugin's editor. No CLAP or VST3 editor was
  loaded, sized, or shown;
- DPI, focus, keyboard, IME, or accessibility behaviour of an embedded
  editor, on any platform;
- whether a generic editor is sufficient for the target users — that is a
  P0-009 question, and P0-009 has not started.
