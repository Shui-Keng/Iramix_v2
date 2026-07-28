# Journaled Session and Undo/Redo — 2026-07-28

Status: local Windows MSVC/GCC and hosted three-OS Java/C++ plus sanitizer
verification passed. P0-012 remains open.

## Scope

This slice connects the production editable session to the append-only command
journal and adds revisioned undo/redo.

For a project-backed edit:

1. validate the expected revision;
2. deep-copy the current session into a candidate controller;
3. apply and validate the command against that candidate;
4. encode the history action plus forward and inverse commands;
5. append and durably flush the journal record;
6. publish the candidate state;
7. return ACK with the new revision.

The journal sequence is the resulting session revision. A stale or invalid
edit never reaches step 5. If append returns an ambiguous failure, the live
session rejects subsequent persistent edits until process reopen.

The command payload schema is independent from the project snapshot, journal
envelope, and Java/C++ IPC schemas. Phase 0 commands cover tempo, add-track,
rename-track, internal remove-track for add undo, and stable-ID restoration on
redo.

Undo and redo do not decrement revision. They append the inverse or forward
operation as a new revision. A new edit after undo clears the redo branch.

## Recovery method

- Create a default project-backed session at revision 1.
- Journal tempo and add-track edits through revision 3.
- Save the revision-3 project snapshot.
- Journal rename and undo through revision 5.
- Destroy the live owner.
- Reopen the revision-3 snapshot and replay revisions 4–5.
- Verify state, undo depth, and redo depth.
- Redo and undo rename, create a new edit, undo that edit, undo add-track, and
  redo add-track through revision 11.
- Verify the restored track retains stable ID 2.
- Reopen again from the still-old revision-3 snapshot and deterministically
  replay the full journal through revision 11.
- Verify one stale command created no journal record.

Raw correctness output:

```text
Journaled session: snapshot_revision=3, replayed_revision=11,
durable_records=10, stale_records=0, undo_depth=2, redo_depth=1,
stable_track_id=2
```

The Java/C++ architecture smoke is extended to journal tempo, undo, redo, save
revision 5, restart the native engine, reconstruct history, undo to revision 6,
and durably save that recovered revision.

## Local durable-ACK timing

Machine:

- Windows 11 Home Single Language, build 26200;
- AMD Athlon Silver 3050U with Radeon Graphics;
- 21.9 GiB visible memory;
- MSVC Release;
- local temporary-directory storage;
- 100 sequential tempo edits;
- every sample includes candidate construction, command encoding, append,
  flush, `_commit`, close, and state publication;
- nearest-rank percentiles.

```text
Journaled edit durable ACK: iterations=100,
p50_ms=2.6163, p95_ms=4.0549, p99_ms=10.1046,
max_ms=12.2645
```

The p99 misses the provisional below-10-ms UI-engine command target by `0.1046
ms`, before Java transport or dummy UI load. No target-pass claim is made.
The result indicates that persistent file handles and/or bounded group commit
must be evaluated while retaining explicit durability semantics.

## Local verification

- MSVC Debug: all six CTest targets passed.
- GCC/MinGW development build: all six CTest targets passed.
- MSVC Release session correctness and timing test passed.
- A manual binary-protocol smoke reached revision 5 through edit/undo/redo,
  restarted the Release engine, reconstructed `undo_depth=2`, applied undo at
  revision 6, and accepted that revision for background save.
- `git diff --check` passed.
- Local Java 21/Gradle verification is unavailable on this machine; the
  existing runtime is older and has no `javac` or Gradle installation. Hosted
  CI is the Java and three-OS portability gate for this slice.

## Hosted verification

Source commit:
`07517f47766ebd6af4474e6017a7d652d7d69dd0`

GitHub Actions:
[`30346059028`](https://github.com/Shui-Keng/Iramix_v2/actions/runs/30346059028)

- Windows Server 2022: six CTest targets and Java 21 architecture/load smoke
  passed.
- macOS hosted runner: six CTest targets and Java 21 architecture/load smoke
  passed.
- Ubuntu hosted runner: six CTest targets and Java 21 architecture/load smoke
  passed.
- ASan/UBSan: all six CTest targets passed without diagnostics.
- TSan: all six CTest targets passed without diagnostics.

The Java architecture smoke includes edit → undo → redo → concurrent saves,
process shutdown, project/journal reopen, recovered undo, and durable save of
the recovered revision.

## Evidence boundary

This proves the algorithm and local implementation for:

- write-ahead publication ordering;
- monotonic edit, undo, and redo revisions;
- stale-command rejection before persistence;
- snapshot-plus-journal state replay;
- retained-history reconstruction;
- deterministic stable-ID redo;
- native project-backed edit integration.

It does not prove:

- the below-10-ms end-to-end command target under UI load;
- physical power-loss behavior of the storage device;
- safe journal compaction after snapshot retention;
- timed autosave and backup rotation;
- full DAW command/state coverage;
- real AWT workspace integration;
- hardware/storage equivalence across the three target OSes.

Hosted CI is portability and sanitizer evidence, not storage-hardware or
power-loss evidence.
