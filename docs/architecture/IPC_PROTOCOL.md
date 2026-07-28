# Java UI–C++ Engine Protocol

Status: Phase 0 draft  
Protocol version: 1

## Boundary

The Java process is an untrusted client of the C++ engine from a real-time and
lifetime perspective. Neither process shares object pointers, ownership, locks,
or language-runtime representations.

```text
Java UI                   C++ Engine
   |  HELLO(version)          |
   |------------------------->|
   |  WELCOME(capabilities)   |
   |<-------------------------|
   |  COMMAND(sequence, data) |
   |------------------------->|
   |  ACK/REJECT(sequence)    |
   |<-------------------------|
   |  SNAPSHOT(revision)      |
   |<-------------------------|
```

## Transport plan

- Phase 0: persistent child-process control session over stdin/stdout.
- Phase 1 control path: local named pipe on Windows and Unix-domain socket on
  macOS/Linux.
- Large immutable payloads: shared memory with explicit generation and length.
- Audio buffers never travel through the UI protocol.
- Native plugin editor surfaces use a separate platform bridge.

The Phase 0 transport is deliberately replaceable. Framing and message
semantics do not depend on process pipes, named pipes, or Unix-domain sockets.

## Version 1 wire header

All integers use unsigned big-endian encoding.

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | Magic: ASCII `IRAM` |
| 4 | 2 | Protocol version |
| 6 | 2 | Message type |
| 8 | 4 | Payload length |
| 12 | 8 | Sequence number |

The fixed header is 20 bytes. Phase 0 limits payloads to 65,536 bytes. Its
control-spike payloads are UTF-8 key/value text; this is not the final session
schema.

Version 1 message type values:

| Value | Type |
|---:|---|
| 1 | `HELLO` |
| 2 | `WELCOME` |
| 3 | `PING` |
| 4 | `ACK` |
| 5 | `SHUTDOWN` |
| 6 | `REJECT` |
| 7 | `SAVE_SESSION` |
| 8 | `POLL_SAVE_COMPLETION` |
| 9 | `SET_TEMPO` |
| 10 | `SESSION_STATE` |

The executable mode `iramix_engine_probe --ipc-stdio` validates
`HELLO/WELCOME`, sequenced `PING/ACK`, and clean `SHUTDOWN/ACK`. When launched
with `--project <path>`, it also advertises `save_session` and
`poll_save_completion`.

`SAVE_SESSION` returns an immediate ACK only after the immutable snapshot is
accepted by the bounded native pipeline. It does not claim durability.
`POLL_SAVE_COMPLETION` returns `none`, `committed`, or `failed` with the
matching session revision. `committed` is published only after serialization,
durable staging, and atomic replacement complete. Phase 0 uses polling to keep
the request/response pipe simple; Phase 1 may replace it with asynchronous
engine events without changing the durability boundary.
The Java UI-facing save API performs that wait on a virtual thread and returns
a `CompletableFuture`; it must not block the AWT event thread. Only an
individual request/response exchange owns the Java exchange lock. Waiting
for durability does not prevent later edit or save commands from being sent.
The exchange guard is a `ReentrantLock`, not a Java `synchronized` monitor:
blocking pipe reads while holding a monitor can pin a Java 21 virtual thread
and starve the reader task. The lock is released between durability polls.

`SESSION_STATE` returns the current native revision and compact state summary.
`SET_TEMPO` carries `expected_revision`; the engine either applies it and
returns the resulting revision or rejects it with the current revision. Save
requests must name the current native revision. The save coordinator may
replace an unaccepted pending snapshot with a newer revision, but it never
drops a revision already accepted by the worker.

## Phase 0 load-test evidence

Recorded on 2026-07-27:

- OS: Windows 11 Home Single Language 64-bit, build 10.0.26200;
- CPU: AMD Athlon Silver 3050U with Radeon Graphics;
- memory: 21.9 GiB;
- engine: CMake Debug build;
- Java: Temurin 21.0.11+10;
- workload: 100 warm-up commands followed by 1,000 measured sequential
  `PING/ACK` commands;
- concurrent UI load: Skia raster surface at 1920x1080, repeatedly drawing 200
  tracks and 2,000 clips on a separate platform thread;
- percentile method: nearest-rank over per-command round-trip duration.

| Commands | UI frames during measurement | p50 | p95 | p99 | Maximum |
|---:|---:|---:|---:|---:|---:|
| 1,000 | 11 | 0.094 ms | 0.247 ms | 1.807 ms | 9.535 ms |

This Windows run is below the provisional 10 ms p99 target. It does not close
the performance gate: the result is from one reference machine and equivalent
macOS and Linux runs are still pending.

The load test also exposed a Windows-specific framing defect: CRT text mode
expanded sequence byte `0x0A` to CRLF. The engine probe now switches stdin and
stdout to binary mode before starting an IPC session. The 1,000-command test
acts as the regression check.

## Message classes

- `HELLO`: UI protocol range, build ID, process ID.
- `WELCOME`: selected version, engine build ID, feature capabilities.
- `COMMAND`: user intent with client sequence number and expected revision.
- `ACK`: accepted command and resulting revision.
- `REJECT`: reason, current revision, and optional recovery instruction.
- `SNAPSHOT`: immutable session or view-model snapshot.
- `TELEMETRY`: bounded, droppable meters and performance counters.
- `ENGINE_STATE`: starting, ready, suspended, recovering, or shutting down.
- `SAVE_SESSION`: enqueue one immutable, revision-matched session snapshot.
- `POLL_SAVE_COMPLETION`: consume the oldest durable save completion.
- `SET_TEMPO`: Phase 0 revisioned-edit exemplar against native session state.
- `SESSION_STATE`: query compact current native session state.

## Ordering and back-pressure

- Commands are ordered per UI connection.
- Persistent commands are never silently dropped.
- Meter and visualization telemetry may be coalesced or dropped.
- Every command carries an expected engine revision.
- Queue saturation produces an explicit status; it never blocks an audio
  callback.

## Schema rules

- All messages begin with a fixed header containing magic, protocol version,
  type, payload length, and sequence.
- Unknown optional fields are ignored.
- Unknown required message types terminate the connection cleanly.
- Schema migration tests retain recorded messages from every public release.
- Java native serialization is forbidden.
- Raw C++ struct layout is forbidden on the wire.

The concrete serialization library for nontrivial payloads remains a Phase 0
decision. FlatBuffers and Cap'n Proto are candidates; the choice must be
benchmarked for schema evolution, Java/C++ ergonomics, and allocation behavior.
