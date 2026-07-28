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

The executable mode `iramix_engine_probe --ipc-stdio` currently validates
`HELLO/WELCOME`, sequenced `PING/ACK`, and clean `SHUTDOWN/ACK`.

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
