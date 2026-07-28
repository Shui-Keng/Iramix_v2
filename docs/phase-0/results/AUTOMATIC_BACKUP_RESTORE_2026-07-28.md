# Automatic Project Backup Restore — 2026-07-28

Status: local Windows MSVC/GCC and Java 21 integration verification passed.
Hosted three-OS and sanitizer verification is pending. P0-012 remains open.

## Scope

This slice connects revisioned backups to `JournaledSession::open`. Recovery
now activates when the committed project is missing, its project envelope is
corrupt, its session schema cannot be decoded, or its snapshot is incompatible
with the command-journal checkpoint.

Candidates are inspected newest-first and must pass every gate:

1. the filename must match the strict revisioned-backup namespace;
2. the checksummed project envelope must load;
3. the session schema must decode and validate;
4. the embedded session revision must equal the filename revision;
5. the revision must be compatible with the journal baseline;
6. replay and undo/redo history reconstruction must succeed;
7. the selected payload must atomically replace the active project.

The active project is not replaced until all validation and replay gates pass.
If replay advances beyond the restored backup revision, the engine immediately
tracks that live snapshot as dirty so fixed-window autosave can advance the
active project rather than leaving recovery dependent on the journal forever.
Recovery state is exposed as `recovered_from_backup`,
`recovered_backup_revision`, and `skipped_backups` in `SESSION_STATE`.

## Durable journal baseline

Compacted history is not necessarily a state delta that can be applied to any
older snapshot. Treating it as one can produce a plausible but incorrect
session.

Every newly created journal and every checkpoint now contains an explicit
baseline record:

```text
checkpoint_baseline = revision of the snapshot required by this history
```

A backup older than that baseline or newer than the journal's final valid
sequence is rejected. Commands newer than the selected backup are replayed
normally, so a revision-2 backup with baseline 2 can recover revision 3 from
the journal.

Legacy journals have no explicit baseline and may already contain synthetic
compacted history. They therefore use a conservative rule: a backup must
exactly match the journal's final valid sequence. Journal-only recovery
also requires an explicit default-session baseline. This may reject a
recoverable old development project, but it cannot silently synthesize an
unverified state.

The baseline record itself uses the same checksummed, strictly ordered,
durably flushed journal envelope. Checkpoint replacement remains staged and
atomic.

## Correctness method

The native session test covers:

- corrupt revision 5 skipped;
- envelope-valid revision 4 rejected because its embedded revision is 3;
- revision 2 selected and atomically restored;
- checkpoint-compatible journal command replay to revision 3;
- schema-valid but checkpoint-stale active project rejected in favor of a
  valid backup;
- journal-only recovery from the explicit default baseline;
- legacy journal rejection of a stale revision-2 backup;
- legacy acceptance once an equally-new revision-3 backup exists.

Raw native result:

```text
Automatic backup restore: checkpoint_baseline=2, backup_revision=2,
replayed_revision=3, corrupt_backups_skipped=1,
revision_mismatches_skipped=1, stale_valid_primaries_rejected=1,
journal_only_recoveries=1, legacy_stale_backups_rejected=1,
active_project_replacements=3
```

The marker adds one record to compacted journals. The same run reported:

```text
Journal checkpoint: original_records=8, initial_baseline_records=1,
baseline_records=1, active_undo_records=2, redo_checkpoint_records=3,
dead_branch_records_removed=6, stable_revision=10,
original_bytes=844, compacted_bytes=244, checkpoint_ms=2.8666
```

This is one warm local Debug timing and is not a storage-performance claim.

The Java architecture smoke closes the engine after revision 5, overwrites the
active project with invalid bytes, relaunches, verifies automatic recovery at
revision 5, then performs and durably saves revision 6:

```text
Automatic backup restore passed after active-project corruption
(restored_revision=5, post_restore_revision=6).
```

## Local verification

- MSVC Debug and GCC/MinGW development builds completed without warnings.
- All six CTest targets passed under both local C++ toolchains.
- Java 21 compiled with `-Xlint:all -Werror`.
- Java/C++ corruption, relaunch, recovery, edit, and save flow passed.

## Evidence boundary

This proves the local selection, compatibility, replay, atomic replacement,
and observability algorithms against deterministic corruption fixtures.

It does not yet prove:

- hosted Windows, macOS, Linux, ASan/UBSan, or TSan portability;
- physical power loss during restore replacement;
- recovery with full or failing storage;
- session coverage beyond the currently serialized state;
- media relinking, MIDI/device state, or plugin-state restoration;
- cold-cache or multi-gigabyte recovery performance.

Hosted CI is portability and sanitizer evidence, not storage-hardware or
power-loss evidence.
