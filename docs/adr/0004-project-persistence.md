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
- Large-file streaming, reference-project open time, and UI-stall budgets pass
  independently.

## Initial evidence

Windows, macOS, Linux, ASan/UBSan, and TSan evidence is recorded in
[`../phase-0/results/PERSISTENCE_RECOVERY_2026-07-28.md`](../phase-0/results/PERSISTENCE_RECOVERY_2026-07-28.md).

The current recovery helper materializes test audio in memory and the snapshot
save call is synchronous. Read-ahead, streaming large-file recovery, worker
integration, final media conversion, and large-project timing remain pending,
so this ADR remains proposed.
