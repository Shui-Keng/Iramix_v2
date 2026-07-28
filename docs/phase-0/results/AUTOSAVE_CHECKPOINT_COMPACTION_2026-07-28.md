# Autosave, Checkpoint, and Journal Compaction — 2026-07-28

Status: local Windows MSVC/GCC and hosted three-OS Java/C++ plus sanitizer
verification passed. P0-012 remains open.

## Scope

This slice adds a production `SessionPersistenceService` around the existing
session save coordinator.

- The first dirty revision starts a fixed autosave window.
- Newer immutable snapshots replace the dirty candidate without indefinitely
  postponing that deadline.
- The engine default is five seconds, matching the acknowledged-edit-loss
  budget; the probe test override is limited to `1..5000 ms`.
- Save serialization, staging, durable flush, and completion polling remain on
  background workers.
- An orderly shutdown flushes the latest dirty revision even when the normal
  autosave deadline has not elapsed.
- A loaded project seeds the service's known durable snapshot revision, while
  journal replay may advance the live revision beyond it.

Every successful project-backed edit, undo, or redo publishes an immutable
dirty snapshot. Java can wait for its timed autosave through the existing
completion poll without first sending `SAVE_SESSION`.

## Checkpoint invariant

Compaction is allowed only when:

```text
durable snapshot revision == current live session revision
```

If the revisions differ, checkpoint returns an error before touching the
journal.

For a valid checkpoint:

1. linearize the active undo stack plus the redo suffix;
2. emit the minimal edit records needed to rebuild that line;
3. emit only the undo records needed to restore the current redo stack;
4. write the compact journal to `.compacting`;
5. durably flush it;
6. atomically replace the old journal.

The old journal stays authoritative unless replacement commits. Compaction
removes abandoned branches and redundant undo/redo toggles, but retains the
ability to undo and redo after process restart.

## Correctness method

Autosave:

- Apply three durably journaled tempo edits.
- Mark each immutable revision dirty across one 30 ms fixed window.
- Verify two dirty snapshot replacements.
- Wait for revision 4 without issuing a manual save.
- Reopen the project at tempo 123 and revision 4.
- Start a separate service with a ten-second interval, mark revision 2 dirty,
  stop immediately, and verify shutdown durably flushes revision 2.

Compaction:

- Build revisions 2–9 from edits A/B/C, undo/redo toggles, two undos, and edit
  D that abandons the old branch.
- Verify checkpoint at stale durable revision 8 is rejected and all eight
  journal records remain.
- Save revision 9, checkpoint, and verify two active records remain.
- Reopen, undo D, save/checkpoint revision 10, and verify the three-record
  compact form reconstructs one undo and one redo.
- Reopen again and redo D to the same state.

## Local MSVC Release results

Machine:

- Windows 11 Home Single Language, build 26200;
- AMD Athlon Silver 3050U with Radeon Graphics;
- 21.9 GiB visible memory;
- local temporary-directory storage;
- nearest behavior is a synthetic correctness screen, not cold-cache storage
  characterization.

```text
Journal checkpoint: original_records=8,
active_undo_records=2, redo_checkpoint_records=3,
dead_branch_records_removed=6, stable_revision=10,
original_bytes=800, compacted_bytes=200,
checkpoint_ms=4.8606

Autosave scheduler: interval_ms=30, edits=3,
autosave_requests=1, dirty_replacements=2,
durable_revision=4, elapsed_ms=56.9689,
shutdown_flush_revision=2
```

The fixed window is not a trailing debounce. On slower GCC runs, the durable
edit sequence crossed the first 30 ms deadline, so revision 3 used one
autosave window and revision 4 used a second. This is expected bounded behavior
and prevents continuous editing from starving persistence.

One manual binary IPC smoke used the Debug engine with a 50 ms interval:

```text
committed;revision=2;bytes=74;serialize_ns=43100;save_ns=6135800

revision=2;tracks=1;undo_depth=1;redo_depth=0;
durable_revision=2;dirty_revision=0;autosave_requests=1;
checkpoint_revision=2
```

After restart, revision 2, durable revision 2, and undo depth 1 were retained.

## Local verification

- MSVC Debug: all six CTest targets passed.
- GCC/MinGW development build: all six CTest targets passed.
- MSVC Release session tests passed.
- Manual binary Java-compatible IPC framing passed autosave, completion,
  checkpoint, shutdown, and reopen.
- Java 21/Gradle is unavailable locally; hosted CI is the Java and portability
  gate.

## Hosted verification

Source commit:
`e64416cc76501010bc718f566b2b3b769d073d46`

GitHub Actions:
[`30347845580`](https://github.com/Shui-Keng/Iramix_v2/actions/runs/30347845580)

- Windows Server 2022: all six CTest targets and Java 21 architecture/load
  smoke passed.
- macOS hosted runner: all six CTest targets and Java 21 architecture/load
  smoke passed.
- Ubuntu hosted runner: all six CTest targets and Java 21 architecture/load
  smoke passed.
- ASan/UBSan: all six CTest targets passed without diagnostics.
- TSan: all six CTest targets passed without diagnostics.

The Java architecture smoke applies revisioned edit/undo/redo, waits for
revision 4 to become durable through the 50 ms timer without `SAVE_SESSION`,
then covers manual coalesced saves, process restart, recovered undo, and a
durable revision-6 save.

## Evidence boundary

This proves the local implementation and recovery algorithm for:

- fixed-window autosave without indefinite debounce;
- latest immutable snapshot replacement;
- background save polling;
- orderly-shutdown dirty flush;
- loaded durable-revision baselines;
- revision-gated atomic checkpoint replacement;
- dead-branch removal;
- undo/redo reconstruction after compaction.

It does not prove:

- physical power-loss behavior during journal replacement;
- cold-cache or reference-storage latency;
- multi-gigabyte project behavior;
- backup rotation and retention;
- plugin-state autosave deadlines;
- complete DAW session coverage;
- hardware/storage equivalence across target operating systems.

Hosted CI is portability and sanitizer evidence, not physical storage or
power-loss evidence.
