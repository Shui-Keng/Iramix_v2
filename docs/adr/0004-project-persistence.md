# ADR-0004: Transactional Project and Recording Staging

Status: Proposed; Phase 0 foundation implemented  
Date: 2026-07-28

## Context

A DAW must preserve acknowledged edits and recorded media across application
failure, operating-system interruption, and partial filesystem writes.
Overwriting the live project or trusting an incomplete audio tail would make a
single crash capable of destroying otherwise valid work.

## Decision

- Project snapshots use a versioned, checksummed envelope.
- A snapshot is written to a sibling staging file, durably flushed, then
  atomically replaces the committed target.
- The command journal is append-only, strictly sequenced, and checksummed per
  record. Durable append completion is the persistent-command ACK boundary.
- Recording staging uses a versioned header and individually sequenced,
  checksummed audio blocks.
- Recovery accepts only the longest valid prefix. Partial, corrupt, or
  out-of-sequence suffixes are discarded.
- Production recovery scans payloads incrementally with a fixed 64 KiB scratch
  buffer. Tail repair truncates only the validated invalid suffix.
- Recording and playback cross the callback boundary through separate,
  preallocated, fixed-capacity SPSC block queues. The recording callback
  rejects a block when full; playback emits silence and counts an underflow
  when no block is ready.
- Filesystem calls, CRC work, durable flushes, and queue allocation remain on
  control or disk-worker threads.
- Project saves use a bounded single-producer pipeline. Each slot owns its
  immutable payload reference from submission through durable completion, so
  an ACK/REJECT cannot be dropped independently from its request.
- A committed completion is published only after the staging flush and atomic
  replacement return successfully. Pipeline saturation and invalid revisions
  are rejected synchronously.
- Session documents use stable non-zero entity IDs and a schema version
  independent from the project envelope. The current Phase 0 schema is v3.
  V1 and v2 migrations supply deterministic defaults or empty collections for
  fields introduced later.
- Schema v3 adds clips, routes, and automation lanes with validated stable
  references. Export to v1/v2 is rejected when it would discard v3 entities.
- Project, journal, recording, and IPC schema versions remain independent.
- No persistence operation may run on an audio callback.

On Windows, atomic replacement uses `MoveFileExW` with replacement and
write-through flags. POSIX uses `rename`, followed by a directory `fsync`.
File durability uses `_commit` on Windows and `fsync` on POSIX.

## Validation required

- Injected failure after staging flush preserves the prior committed project.
- A complete staging snapshot can recover a missing or corrupt target.
- Journal recovery truncates a partial suffix before the next append.
- Forced process termination during recording preserves every durably flushed
  block.
- Corrupt recording blocks and their suffix are rejected.
- Recording and read-ahead queue saturation remains bounded and observable.
- Allocation-tracking hooks report zero callback allocation/deallocation and
  zero tracked blocking locks for enqueue, dequeue, and underflow paths.
- Async-save completion order matches accepted revision order, injected
  replacement failures return explicit rejection, and the committed project
  preserves the last successful revision.
- Current-schema round trips and every retained legacy fixture migrate forward.
- Large-file streaming, reference-project open time, and UI-stall budgets pass
  independently.

## Initial evidence

Windows, macOS, Linux, ASan/UBSan, and TSan evidence is recorded in
[`../phase-0/results/PERSISTENCE_RECOVERY_2026-07-28.md`](../phase-0/results/PERSISTENCE_RECOVERY_2026-07-28.md).

The production scanner/reader and bounded disk workers are implemented. The
legacy `recoverRecording` helper still materializes samples for tests and
small imports, while production streaming uses `scanRecording` and
`RecoverableRecordingReader`.

The project save call now runs behind a bounded worker pipeline, and the
stable-ID session DTO plus v1/v2-to-v3 migration fixtures are implemented.
Session
serialization is still prepared before submission; submit-latency evidence
therefore does not include serialization cost.

Final media conversion, complete DAW session coverage, live engine integration,
full-scale large-recording timing, and reference-project open measurements
remain pending, so this ADR remains proposed.
