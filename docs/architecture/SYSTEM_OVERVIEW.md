# System Overview

## Architectural boundaries

```text
Java UI Process
  interaction + view state + Skiko/Skia
          |
  commands / immutable snapshots
          |
          v
C++ Engine Process
  command dispatcher ---- Undo journal ---- Project persistence
          |
  mutable session model
          |
  graph compiler
          |
  immutable render plan
          |
  real-time coordinator ---- DSP workers ---- Disk streaming
          |
  audio device backend
```

Plugin scanning, plugin execution, waveform analysis, indexing, and crash
handling live outside the real-time engine boundary.

## Thread and process model

### Audio callback

- Owns the current immutable render plan.
- Reads preallocated command and event queues.
- Performs no allocation, locking, filesystem access, logging, or destruction.
- Publishes compact telemetry to a lock-free queue.

The current Tier-1 executor implements one atomic plan-pointer load per block,
pre-resolved node and buffer pointers, fixed-capacity MIDI, and an
acknowledged-generation reclamation protocol. Each prepared plan co-owns its
node objects, preventing topology replacement from invalidating nodes still
in use by the callback. The Windows WASAPI render path now runs the initial
device-input, track, gain, mixer, and output node chain and copies the planar
graph result into the actual device buffer. Other platform backends and live
capture remain pending.

Scalar node parameters use a bounded control-to-audio SPSC queue with absolute
sample timestamps. Value, linear-ramp, and additive-modulation events share
the same queue. Ramps retain state across process blocks; modulation values
can change at every sample and persist until replaced. The callback routes due
events into preallocated per-node buffers and nodes apply them at sample
offsets. Queue saturation, late events, unknown targets, and per-node overflow
are explicit counters. A seek or loop timeline discontinuity must flush and
rebase the queue while processing is stopped.

The outermost callback scope enables hardware denormal protection (x86 FTZ and
DAZ, or AArch64 FZ where supported), counts protected callback entries, and
restores the previous floating-point control state on exit.

General real-time commands use a bounded sequenced SPSC queue and return an
ACK or REJECT containing the applied plan generation. A full completion queue
backpressures command execution, so an accepted command remains pending rather
than silently losing its completion. Compact per-block telemetry uses a
separate bounded queue; it may drop under saturation, with every drop counted.

### C++ control thread

- Applies accepted UI commands to the mutable session.
- Compiles render plans outside the audio callback.
- Atomically publishes a new plan at a buffer boundary.
- Retires old plans away from the audio thread.

### DSP worker pool

- Executes only independent graph partitions.
- Uses fixed-capacity work queues.
- Obeys the audio deadline; incomplete optional work must never block the
  coordinator indefinitely.

### Java UI thread

- Produces versioned commands and consumes immutable snapshots.
- Never owns C++ pointers or native engine objects.
- Uses virtualized layout and cached visual assets.
- May allocate, but allocation rate and GC pauses remain measured UI budgets.

### UI–engine control session

- Uses a persistent, sequenced binary protocol.
- Starts with version negotiation and advertised engine capabilities.
- Applies timeouts on the Java side and terminates an unresponsive probe.
- Currently runs over child-process stdin/stdout; Phase 1 replaces only the
  transport with named pipes or Unix-domain sockets.
- Never carries audio buffers and never runs on the audio callback.

### Service workers and processes

- Stream audio from disk.
- Drain recording blocks from a preallocated callback-to-worker SPSC queue.
- Fill playback read-ahead through a preallocated worker-to-callback SPSC
  queue; callback underflow produces silence and an explicit counter.
- Build waveform and spectral caches.
- Scan and host plugins.
- Index browser content.
- Persist project commands, immutable snapshots, and backups.
- Publish durable project-save ACK/REJECT records through a bounded pipeline;
  pipeline saturation is an explicit control-thread outcome.
- Validate and serialize immutable session snapshots on the save worker, not
  on the Java UI or C++ command-dispatch thread.

## Platform boundaries

The C++ engine isolates:

- audio devices;
- MIDI devices;
- filesystem and file watching;
- process and shared-memory management;
- plugin editor embedding;
- plugin formats;
- native engine diagnostics.

The Java process owns normal desktop UI and accessibility. A small native
window bridge remains available for third-party plugin editors and OS features
that cannot be represented safely through AWT.

## Rendering and application shell

The Java process uses Skiko to create the AWT window and render with Skia.
Iramix-owned Java scene interfaces isolate product widgets from Skiko details.

The validation sequence is:

1. Java/Skiko reference scene on Windows.
2. Matching macOS and Linux reference scenes.
3. HiDPI, font shaping, IME, and accessibility tests.
4. Dense arrangement frame-time benchmark.
5. Software fallback and renderer recovery.

Backend selection must remain runtime-observable, and renderer failure must
degrade gracefully rather than affect audio.

## Audio backends

- Windows: ASIO primary, WASAPI fallback
- macOS: Core Audio
- Linux: PipeWire primary, JACK and ALSA compatibility paths

The engine processes planar 32-bit floating-point audio initially. File and
device formats are converted only at the boundaries.

## Persistence

The working project is a directory containing a transactional session database,
media, caches, freeze data, and backups. All entities use stable identifiers.
Database work is forbidden on real-time threads.

The project schema, command journal, and IPC schema are versioned independently.
Every released schema has forward migration tests.

The Phase 0 persistence foundation writes checksummed snapshots to a durable
sibling staging file before atomic replacement. Persistent commands use a
strictly sequenced, checksummed append-only journal whose durable append is the
ACK boundary. Recording staging uses sequenced, checksummed audio blocks;
recovery accepts only the longest valid prefix. The production scanner
validates payloads incrementally with a fixed 64 KiB scratch buffer and can
truncate only the invalid suffix. Recording and playback use fixed-capacity
SPSC queues whose callback-facing operations perform copies plus lock-free
atomic publication only. Filesystem and durable-flush work runs on disk worker
threads.

Project snapshots can be submitted either as immutable byte payloads or as
immutable session documents to bounded save workers. The session worker
performs validation and schema serialization before durable staging. A
pipeline slot is retained until the control thread consumes its completion.
The worker publishes `committed` only after serialization, durable staging,
and atomic replacement succeed.

The Phase 0 session DTO uses globally unique stable entity IDs and an
independently versioned binary schema. Schema v3 stores revision, sample rate,
tempo, tracks, clips, routing, and automation. References are validated on
save and load. V1/v2 migrations supply deterministic defaults and empty v3
collections; lossy legacy export is rejected. Runtime audio nodes are not
serialized.

The Java/C++ Phase 0 probe now exercises revisioned save acceptance and durable
completion through IPC. Its native snapshot is still a minimal engine-probe
fixture rather than the full mutable DAW session. Complete DAW session
coverage, final media conversion, cold-cache/reference-hardware timing, and
media/plugin restoration remain pending.
