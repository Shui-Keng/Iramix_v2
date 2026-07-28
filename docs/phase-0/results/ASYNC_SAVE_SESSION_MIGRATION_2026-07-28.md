# Async Save and Session Migration — 2026-07-28

Status: P0-012 save/session slice verified locally and on hosted three-OS CI
plus sanitizers; Week 6 remains open.

## Scope

This slice adds:

1. a bounded asynchronous project-save pipeline;
2. explicit request saturation, invalid-revision, committed, and failed
   outcomes;
3. a deterministic stable-ID session document with an independent schema
   version;
4. a retained schema-v1 fixture that migrates to schema v2;
5. current-schema and migrated project round-trip tests.

The save pipeline is single-producer/control-thread to single-worker. A slot
remains occupied from request publication until its completion is consumed.
This couples capacity for requests and completions: the worker cannot silently
drop an ACK/REJECT, and unconsumed completions naturally backpressure new
submissions.

The caller supplies shared ownership of an immutable byte vector. Submission
does not copy or serialize the payload. The worker writes the checksummed
staging file, performs the durable flush and atomic replacement, then publishes
`committed`. Any failure publishes `failed` with a bounded diagnostic.

## Session schema

The current Phase 0 session schema is v2. It contains:

- non-zero session revision;
- sample rate and tempo;
- ordered tracks with non-zero unique stable IDs;
- audio, instrument, group, effect-return, and master track types;
- gain, color, and name.

Schema v1 lacks sample rate and track color. Its migration supplies 48 kHz and
color zero deterministically. Unknown future schemas, duplicate IDs, truncated
fields, trailing bytes, invalid track types, and invalid numeric values are
rejected.

This DTO is deliberately separate from runtime audio nodes and pointers. It is
the migration/round-trip foundation, not the complete DAW session schema or a
decision to use this binary layout as the final session database.

## Method

Correctness screening:

- commit a base project synchronously;
- queue one save with injected failure after staging flush;
- queue a second valid revision;
- prove the third request reports `full`;
- prove a duplicate revision is rejected;
- prove no completion or project change exists before worker processing;
- drain the worker and verify ordered `failed` then `committed` completions;
- reopen the project and verify only the successful revision committed.

Load screening:

- serialize and submit 200 distinct session revisions through an eight-slot
  pipeline while the disk worker is active;
- consume every completion and require all are `committed`;
- measure only the duration of each accepted `trySubmit` call;
- reopen the final project and require revision 200.

Migration screening:

- serialize and deserialize schema v2 with three stable-ID tracks;
- serialize a schema-v1 fixture and migrate it to v2 defaults;
- round-trip the v2 bytes through the checksummed project envelope;
- reject an unknown future schema and duplicate stable IDs.

## Local results

Source commit: `e73f5f2ca02a4028558527ec23bd1ea03095fe8a`

GCC/Ninja and MSVC passed all five CTest targets on Windows. Raw MSVC additions:

```text
Session document: current_schema=2, tracks=3, stable_ids=3,
v1_migrations=1, unknown_schemas_rejected=1, project_round_trips=1
Async project saver: revisions=200, capacity=8,
committed_completions=200, submit_p50_ms=0.0005,
submit_p95_ms=0.0051, submit_p99_ms=0.0061,
submit_max_ms=0.0147, injected_failures=1,
explicit_full_rejections=1
```

The submit p99 is below the 16 ms UI-stall budget, but it measures only bounded
pipeline publication of an already-built immutable payload. Serialization,
Java-to-C++ IPC, queue-wait time under sustained saturation, and durable
completion latency are excluded and must be measured separately.

## CI and sanitizer results

GitHub Actions:
[`30339587974`](https://github.com/Shui-Keng/Iramix_v2/actions/runs/30339587974)

The complete suite passed on Windows, macOS, and Ubuntu. ASan/UBSan and TSan
also passed all five targets without diagnostics. The Windows job emitted only
the known non-blocking Gradle cache-path warnings after its successful build
and tests.

Hosted CI validates portability and sanitizer behavior. Its save timings are
not reference-storage performance evidence.

## Evidence boundary

This proves the first stable-ID schema round trip, one explicit migration,
bounded save backpressure, durable ACK ordering, failure rejection, and final
revision recovery for the test corpus.

It does not prove:

- full arrangement, launcher, clip, automation, routing, plugin, or device
  state coverage;
- migration from an actual public Iramix release;
- serialization or complete UI save-path latency;
- large-project open below five seconds;
- autosave policy, coalescing, backup retention, or free-space handling;
- recovery under OS power loss or storage hardware failure;
- live Java UI and engine-session integration.
