# Reference Session Schema v3 — 2026-07-28

Status: P0-012 schema/reference benchmark verified locally and on hosted
three-OS CI plus sanitizers; Week 6 remains open.

Schema v3 has since been extended by
[`COMPLETE_SESSION_STATE`](COMPLETE_SESSION_STATE_2026-07-28.md), which adds
media references, MIDI, device configuration, and plugin state as v4. The
results below describe the v3 format and its smaller reference workload.

## Scope

Session schema v3 extends the editable persistence DTO with:

- clips referencing stable track and external media-source IDs;
- routing records referencing stable source and destination track IDs;
- automation lanes referencing stable track IDs and stable parameter IDs;
- ordered sample-position/value automation points;
- globally unique non-zero IDs across tracks, clips, routes, and lanes.

Validation rejects dangling track references, duplicate entity IDs, invalid
numeric values, zero-length clips, self-routes, unordered automation points,
unknown schemas, truncated input, and trailing input.

V1 and v2 documents migrate to v3 with empty clip/route/automation
collections. Export to v1 or v2 is rejected whenever v3 entities exist, so a
migration or compatibility tool cannot silently discard newer project data.

## Reference workload

The synthetic reference project follows the declared UI/persistence scale:

- 200 tracks;
- 2,000 clips;
- 199 track-to-master routes;
- 40 automation lanes;
- 1,000 points per lane, 40,000 points total.

The serialized project envelope is 652,772 bytes. Twenty iterations measure:

1. schema serialization into a fresh byte vector;
2. project-envelope load, checksum verification, and full schema decode.

Percentiles use nearest-rank. With 20 samples, p99 is the observed maximum.
The open loop runs in one process against a local temporary file, so operating
system cache effects are expected.

## Local MSVC Release result

Source commit: `d256c37ecb7144f0cbc5db8614354bdbf42f879d`

```text
Session document: current_schema=3, tracks=3, clips=2, routes=1,
automation_lanes=1, v1_migrations=1, v2_migrations=1,
unknown_schemas_rejected=1, project_round_trips=1
Reference project: tracks=200, clips=2000, routes=199,
automation_lanes=40, automation_points=40000, project_bytes=652772,
iterations=20, serialize_p50_ms=3.4328, serialize_p95_ms=17.1632,
serialize_p99_ms=21.4607, serialize_max_ms=21.4607,
open_p50_ms=14.7268, open_p95_ms=44.0381,
open_p99_ms=61.7546, open_max_ms=61.7546,
truncated_projects_rejected=1
```

The synthetic warm-local open p99 is below the five-second target. The
serialize p95/p99 exceed the 16 ms UI-frame budget, which reinforces the
architecture requirement that serialization execute away from the Java UI
thread rather than weakening the budget.

## CI and sanitizer results

GitHub Actions:
[`30341115944`](https://github.com/Shui-Keng/Iramix_v2/actions/runs/30341115944)

The complete suite passed on Windows, macOS, and Ubuntu. ASan/UBSan and TSan
also passed without diagnostics. These hosted jobs validate portability,
format behavior, and sanitizer coverage; their filesystem timing is not used
as reference performance evidence.

## Evidence boundary

This proves bounded format validation, stable referential integrity, v1/v2
migration to v3, non-lossy legacy-export behavior, and local warm-open behavior
for the declared entity counts.

It does not prove:

- cold-cache open behavior on declared reference hardware;
- sessions containing real audio media, MIDI notes, devices, plugins, plugin
  state, launcher scenes, tempo maps, or undo history;
- Java-to-C++ snapshot construction and IPC cost;
- background serialization scheduling or cancellation;
- storage behavior under low space, antivirus contention, network filesystems,
  or device failure;
- a production database/storage-format decision.
