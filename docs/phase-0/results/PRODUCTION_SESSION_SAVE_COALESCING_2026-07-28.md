# Production Session Ownership and Save Coalescing — 2026-07-28

Status: Windows local screening passed; hosted three-OS CI pending for this
source revision. P0-012 remains open.

## Scope

This slice replaces the engine probe's synthetic save fixture with a native
control-thread-owned `SessionController`.

The controller:

- starts at revision 1 with a stable-ID master track;
- applies edits only when `expectedRevision` matches;
- advances revision only after a valid edit applies;
- owns stable-ID allocation across project reload;
- publishes deep-copied immutable `SessionDocument` snapshots;
- remains separate from immutable real-time render plans.

The implemented edit surface covers tempo, track creation, and track rename.
The Java/C++ smoke exercises revision query and tempo editing before requesting
a save, so persistence now consumes state owned by the engine rather than a
save-handler fixture.

## Save coalescing

`SessionSaveCoordinator` permits:

- one immutable revision already accepted by the serialization worker;
- one replaceable latest revision waiting behind it.

If revisions 2, 3, and 4 arrive as a burst:

1. revision 2 is accepted by the worker and must complete;
2. revision 3 becomes the pending snapshot;
3. revision 4 replaces revision 3 before worker acceptance;
4. revision 2 and revision 4 receive durable outcomes;
5. revision 4 contains all edits from revision 3.

No accepted request is discarded. Coalescing applies only to a snapshot that
has not entered the worker. Duplicate latest requests are idempotent and older
revisions are rejected.

## Correctness method

- Verify the default master track and initial revision.
- Apply tempo, add-track, and rename-track edits.
- Reject stale expected revisions and invalid tempo without advancing state.
- Keep an old immutable snapshot alive and prove later edits do not mutate it.
- Reload a document and prove stable-ID allocation resumes above existing IDs.
- Queue revisions 2, 3, and 4 before worker start and prove only 2 and 4 are
  submitted.
- Reopen the project at revision 4 with the latest tempo.
- Start a second coordinator before submission and prove its completion
  remains live.
- Launch Java against the engine, query revision 1, apply tempo at expected
  revision 1, request save revision 2, apply another tempo edit while that
  durability wait is active, then request and durably cover revision 3.

## Local MSVC Release results

The synthetic reference document contains 200 tracks, 2,000 clips, 199 routes,
40 automation lanes, and 40,000 automation points. Twenty deep-copy immutable
snapshots used nearest-rank percentiles:

```text
Session controller: initial_revision=1, final_revision=4,
revision_conflicts=1, invalid_edits=1, stable_ids=3,
immutable_snapshots=1

Session save coordinator: requested_revision=4,
durable_revision=4, submitted=2, coalesced=2,
duplicate_requests=1, stale_rejections=1

Reference session snapshot: tracks=200, clips=2000,
automation_points=40000, iterations=20,
snapshot_p50_ms=0.28, snapshot_p95_ms=0.6029,
snapshot_p99_ms=1.1254, snapshot_max_ms=1.1254
```

Snapshot creation is below the provisional 16 ms UI-stall budget in this warm
local Release screening. This does not establish cold-cache or reference
hardware performance.

The Java/C++ Release smoke produced one raw correctness sample:

```text
bytes=74, serialize_ns=11400, save_ns=4778500,
production_revision=3, covered_revision=2
```

It is not a latency percentile. Five consecutive post-fix MSVC Release smoke
runs passed; their durable-save samples varied from approximately 4.0 ms to
9.4 ms.

The concurrent revision-2/revision-3 smoke initially exposed a Java 21
liveness defect: a virtual thread waited for pipe I/O while holding a
`synchronized` monitor, which can pin its carrier and starve the reader task.
The transport now serializes individual exchanges with `ReentrantLock`.
Durability polling releases that lock between frames, so later edits and save
requests continue. The two overlapping save futures are the regression test.

## Local verification

- MSVC Debug: all six CTest targets passed.
- GCC/MinGW: all six CTest targets passed.
- MSVC Release session tests passed.
- Java 21 compiled with `-Xlint:all -Werror`.
- Java/C++ architecture smoke passed against Debug, Release, and three
  consecutive GCC/MinGW engine launches.
- Five consecutive concurrent-save MSVC Release smoke runs passed after the
  virtual-thread pinning fix.

## Evidence boundary

This proves native session ownership, optimistic revision checks, immutable
snapshot isolation, stable-ID continuation, bounded latest-revision
coalescing, and live Java edit → native state → durable save integration.

It does not yet prove:

- the complete V1 session state or editing command surface;
- command-journal integration, undo, or redo for these live edits;
- automatic time-based autosave scheduling;
- snapshot behavior during sustained edits from the real AWT workspace;
- MIDI, devices, plugins, media references, launcher state, or comping;
- reference-storage and power-loss behavior.

Hosted CI can validate portability and sanitizers but is not storage-hardware
or audio-hardware evidence.
