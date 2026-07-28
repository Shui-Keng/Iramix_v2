# Revisioned Project Backup Rotation — 2026-07-28

Status: local Windows MSVC/GCC and Java 21 integration verification passed.
Hosted three-OS and sanitizer verification is pending. P0-012 remains open.

## Scope

This slice adds revisioned project backups to the production session save
worker.

- The active project is committed first through the existing checksummed,
  staging-flush, atomic-replacement boundary.
- Only after that commit succeeds does the same worker write a backup from the
  exact same immutable serialized payload.
- Backups live beside the project in `<project>.backups/` and use sortable
  names such as `revision-00000000000000000042.irpx`.
- The production default retains ten recognized revisions.
- A repeated save of the same revision replaces that revision's backup
  transactionally rather than creating an ambiguous duplicate.
- Pruning begins only after the new backup commits.
- Only the strict `revision-<20 digits>.irpx` namespace is eligible for
  pruning. Unknown files and `.saving` transaction files are ignored.

Backup work runs after serialization and the primary durable write, on the
bounded save worker and before it accepts the next pipeline slot. It therefore
cannot accidentally copy a newer active project after another save races
ahead.

## Failure semantics

Primary durability and backup health are separate outcomes:

```text
primary commit fails   -> save completion is failed; no backup is attempted
primary commit passes  -> save completion remains committed
backup commit fails    -> backup_status=failed
pruning fails          -> backup_status=retention_warning
all backup work passes -> backup_status=committed
```

A backup failure never rewrites a successful primary ACK as failed. The
completion also reports backup duration, pruned count, retained count, and a
separate backup detail string.

The IPC completion now includes:

```text
backup_status
backup_ns
backup_pruned
backup_retained
backup_detail (only when non-empty)
```

Java exposes those values through `SessionSaveResult`.

## Correctness method

The native persistence test:

1. commits revisions 1–6 with retention set to three;
2. verifies only revisions 4, 5, and 6 remain;
3. places an unrelated file in the backup directory and verifies pruning does
   not remove it;
4. adds a corrupt but recognized revision 7;
5. verifies recovery skips revision 7 and loads envelope-valid revision 6;
6. makes the configured backup directory an ordinary file;
7. verifies the primary project still commits while the completion reports
   `backup_status=failed`.

Raw native result:

```text
Project backups: committed=6, retention=3, pruned=3,
corrupt_skipped=1, unknown_files_preserved=1,
primary_ack_isolated_from_backup_failure=1
```

The local Java/C++ architecture smoke exercises real autosave, coalesced save,
restart, recovered undo, and another durable save with the production default:

```text
Persistent Java/C++ IPC and background save passed on Windows
(bytes=74, serialize_ns=20500, save_ns=4790500,
backup_status=committed, backup_ns=3666500, backup_retained=2,
production_revision=5, covered_revision=4).
```

These are raw timings from one Debug run and are not performance claims.

## Local verification

- MSVC Debug and GCC/MinGW development builds completed without warnings.
- All six CTest targets passed under both local C++ toolchains.
- Java 21 compiled with `-Xlint:all -Werror`.
- Java/C++ architecture smoke passed and verified a committed backup.
- `git diff --check` passed; line-ending notices are repository checkout
  behavior, not whitespace errors.

## Evidence boundary

This proves backup creation ordering, bounded revision retention, unknown-file
preservation, corrupt-envelope skipping, and primary/backup failure isolation
in local synthetic tests.

It does not yet prove:

- hosted Windows, macOS, Linux, ASan/UBSan, or TSan portability;
- automatic restoration of the active project from a backup;
- session-schema and embedded-revision validation during restore;
- physical power-loss behavior during backup commit or pruning;
- cold-cache or slow/full-storage behavior;
- complete media, MIDI, device, or plugin-state restoration.

`recoverNewestProjectBackup` is deliberately a selection primitive: it
validates the project envelope and returns the filename revision. A future
restore orchestrator must also decode the session schema, require its embedded
revision to match the filename, and reconcile the command-journal checkpoint
baseline before replacing the active project.

Hosted CI is portability and sanitizer evidence, not storage-hardware or
power-loss evidence.
