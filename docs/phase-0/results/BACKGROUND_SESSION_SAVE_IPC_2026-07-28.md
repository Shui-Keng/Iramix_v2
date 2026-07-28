# Background Session Save and IPC — 2026-07-28

Status: Windows local screening passed; hosted three-OS CI pending for this
source revision. P0-012 remains open.

## Scope

This slice moves session validation and serialization off the caller thread.
`AsyncSessionSaver` accepts shared ownership of an immutable
`SessionDocument`, then performs these operations on its single worker:

1. validate and serialize the versioned session schema;
2. build the checksummed project envelope;
3. durably flush a sibling staging file;
4. atomically replace the committed project;
5. publish a bounded, revisioned committed/failed completion.

The bounded slot remains occupied until its completion is consumed. A full
pipeline, mismatched/non-monotonic revision, stopped worker, serialization
failure, and durable-save failure are distinct observable outcomes.

The engine probe also exposes Phase 0 `SAVE_SESSION` and
`POLL_SAVE_COMPLETION` messages. Java sends a revisioned request, receives an
immediate acceptance ACK, and polls until the matching durable completion.
This exercises a real Java-process → C++ worker → project-file → Java
completion path. The Java UI-facing entry point returns a `CompletableFuture`
backed by a virtual thread, so waiting for durability does not block the AWT
event thread.

## Correctness method

- Queue an invalid schema, an injected durable-write failure, and a valid
  revision before starting the worker.
- Stop the worker and prove shutdown drains all accepted requests.
- Consume ordered completions: serialization reject, durable-save reject,
  then committed ACK.
- Reject a request whose command revision differs from its immutable snapshot.
- Prove a full three-slot pipeline returns explicit backpressure.
- Reopen the committed project and verify the final successful revision.
- Submit 20 copies of the synthetic reference session through an active
  four-slot worker and require ordered completion of all 20.
- Launch the C++ engine probe from Java, save revision 1 to a temporary project,
  validate the completion metrics, and verify the file exists.

## Local MSVC Release results

Host: Windows 11, local warm filesystem. Percentiles use nearest-rank over 20
reference-session saves.

Reference session:

- 200 tracks;
- 2,000 clips;
- 199 routes;
- 40 automation lanes;
- 40,000 automation points;
- serialized payload greater than 600,000 bytes.

```text
Async session saver: revisions=20,
reference_tracks=200, reference_clips=2000,
reference_automation_points=40000,
submit_p50_ms=0.0005, submit_p95_ms=0.0012,
submit_p99_ms=0.0014,
serialize_worker_p50_ms=2.8118,
serialize_worker_p95_ms=3.7502,
serialize_worker_p99_ms=3.8251,
durable_save_worker_p50_ms=13.5442,
durable_save_worker_p95_ms=20.8262,
durable_save_worker_p99_ms=28.8445,
ordered_completions=20,
serialization_failures=1, injected_save_failures=1
```

The Java/C++ smoke used a small one-track session and produced one raw sample:

```text
bytes=74, serialize_ns=46700, save_ns=4652800
```

That single IPC sample is correctness evidence only. It is not a percentile or
a performance claim.

The immutable snapshot handoff p99 is below the 16 ms UI-stall budget.
Serialization and durable-save tails occur on the worker and therefore do not
stall the caller. The 28.8445 ms durable-save p99 is reported as measured; it
is not rounded into a claim that complete save durability finishes within one
UI frame.

## Local verification

- MSVC Debug: all five CTest targets passed.
- MSVC Release persistence executable passed.
- Java 21 compiled with `-Xlint:all -Werror`.
- Java architecture smoke passed against the MSVC Debug and Release engine
  probes.

## Evidence boundary

This proves bounded background schema serialization, ordered durable
completion, shutdown draining, explicit failure/backpressure, and the first
live Java/C++ save-command path.

It does not yet prove:

- that the saved snapshot is built from the production mutable DAW session;
- complete MIDI, device, plugin, launcher, comping, and media-reference state;
- save coalescing or autosave scheduling under continuous edits;
- cancellation, free-space handling, backup retention, or power-loss behavior;
- cold-cache or reference-storage performance;
- hardware behavior on macOS/Linux.

Hosted CI can verify source portability and sanitizers, but it is not
reference-storage or audio-hardware evidence.
