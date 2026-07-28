# Complete Session State (Schema v4) — 2026-07-28

Status: P0-012 media/MIDI/device/plugin session state verified locally and
on hosted three-OS CI plus sanitizers; Week 6 remains open.

## Scope

Session schema v4 closes the four state gaps
[`REFERENCE_SESSION_SCHEMA_V3`](REFERENCE_SESSION_SCHEMA_V3_2026-07-28.md)
listed as unproven, by extending the persistence DTO with:

- **media references** — external audio sources carrying a path, content
  hash, frame count, sample rate, and channel count;
- **MIDI** — sample-domain note sequences with position, length, channel,
  key, and velocity;
- **device configuration** — backend, device IDs, sample rate, buffer
  frames, and channel counts;
- **plugin state** — per-track plugin slots with format, restorable
  identifier, bypass flag, and an opaque bounded state blob.

Media sources and MIDI sequences share the clip source ID space, so every
clip resolves to exactly one declared source. Referential integrity is now
total across tracks, clips, routes, automation lanes, media sources, MIDI
sequences, and plugins.

## Validation rules added

Validation rejects:

- clips referencing an undeclared media source or MIDI sequence;
- duplicate IDs across the extended entity set;
- MIDI notes that are unordered or duplicated under the total order
  (position, channel, key), zero-length, or outside channel 0–15,
  key 0–127, or velocity `(0, 1]`;
- non-finite MIDI velocities and note ranges that overflow the frame space;
- media sources with an out-of-bounds sample rate, an implausible channel
  count, or an oversized path;
- plugins with an empty identifier, a dangling track reference, an unknown
  format, or a duplicate slot index on the same track;
- plugin state above 16 MiB per instance or 256 MiB per document;
- device configurations with an unknown backend, an out-of-range buffer
  size or channel count, or a device ID without an explicit backend.

The plugin state blob is never interpreted by the host; it is bounded on
both encode and decode and covered by the existing project checksum.

## Migration and legacy export

V1 and v2 continue to migrate as before. V3 documents migrate to v4 by
naming each distinct clip source as an **unresolved media placeholder**: a
media source with the clip's source ID and no path or audio properties.
That keeps the migrated document referentially total and gives a later
relink pass a defined slot to write the located path into. If a synthesized
placeholder ID collides with an existing entity ID the migration fails
closed with a duplicate-ID error rather than silently rewriting references.

Export to v3 or older is rejected whenever the document holds MIDI
sequences, plugins, a non-default device configuration, or any media source
that is described (has a path, hash, or audio properties) or is referenced
by no clip. A set of purely placeholder, clip-referenced media sources is
discardable without data loss, so that case is allowed — v3 clips already
encode the same identities.

## Reference workload

The synthetic reference project grows to exercise the new state:

- 200 tracks;
- 2,000 clips;
- 1,500 audio media sources;
- 500 MIDI sequences, 200 notes each, 100,000 notes total;
- 199 track-to-master routes;
- 40 automation lanes, 1,000 points per lane, 40,000 points total;
- 200 plugins, one per track, 4,096 bytes of state each,
  819,200 plugin-state bytes total;
- one fully specified device configuration.

The serialized project envelope grows from 652,772 bytes (v3 workload) to
4,430,707 bytes. Twenty iterations measure schema serialization and
project-envelope load plus full decode, as before. Percentiles use
nearest-rank, so with 20 samples p99 is the observed maximum. The open loop
runs in one process against a local temporary file, so operating system
cache effects are expected.

## Local Release result

Source commit: `2c8277ed2d523e259d4771ece87d4580abd779d4`
(working tree, schema v4 change applied)

Toolchain: GCC 15.2.0 (MSYS2 UCRT64), Ninja, `CMAKE_BUILD_TYPE=Release`.

**This is a different compiler from the MSVC Release figures recorded in
the v3 reference document, so the two runs are not directly comparable as
a like-for-like regression.** The v4 numbers below stand on their own
against the budget; a same-toolchain comparison against v3 has not been
run.

```text
Session document: current_schema=4, tracks=3, clips=2, routes=1,
automation_lanes=1, media_sources=1, midi_sequences=1, midi_notes=3,
plugins=2, plugin_state_bytes=4, v1_migrations=1, v2_migrations=1,
v3_migrations=1, unknown_schemas_rejected=1, project_round_trips=1

Reference project: tracks=200, clips=2000, routes=199,
automation_lanes=40, automation_points=40000, media_sources=1500,
midi_sequences=500, midi_notes=100000, plugins=200,
plugin_state_bytes=819200, project_bytes=4430707, iterations=20,
serialize_p50_ms=4.3348, serialize_p95_ms=8.7554,
serialize_p99_ms=17.9294, serialize_max_ms=17.9294,
open_p50_ms=57.3487, open_p95_ms=89.093, open_p99_ms=129.017,
open_max_ms=129.017, truncated_projects_rejected=1
```

Warm-local open p99 of 129 ms remains far below the five-second target
even with a 6.8x larger envelope. Serialize p95/p99 still exceed the 16 ms
UI-frame budget, which continues to require serialization off the Java UI
thread rather than a weakened budget.

The full local suite passes:

```text
100% tests passed out of 6
```

Session-side snapshot cost is unchanged in shape
(`snapshot_p99_ms=0.5797` over 20 iterations at the reference scale).

## CI and sanitizer results

GitHub Actions:
[`30354240580`](https://github.com/Shui-Keng/Iramix_v2/actions/runs/30354240580)

All five jobs passed on commit `45fb769`:

| Job | Result |
|---|---|
| build — windows-2022, `windows-msvc` | passed (6/6 tests, persistence 14.10 s) |
| build — macos-latest, `dev` | passed |
| build — ubuntu-latest, `dev` | passed |
| sanitizers — ASan/UBSan | passed, no diagnostics |
| sanitizers — TSan | passed, no diagnostics |

Each build job also ran `gradle check`, so the Java UI and engine
handshake build against schema v4 unchanged.

These hosted jobs validate portability, format behavior, and sanitizer
coverage for the extended entity set. They are not performance evidence:
the build matrix runs `ctest` without `--verbose`, so no benchmark output
is emitted, and the Windows leg builds Debug rather than Release. The v4
reference timings above remain the only performance figures, and they are
GCC/UCRT64 Release as noted.

## Consumer changes

`SessionController` now includes media sources, MIDI sequences, and
plugins in its monotonic stable-ID allocation, and refuses to remove a
track that a plugin still targets — matching the existing refusal for
clips, routes, and automation lanes.

## Evidence boundary

This proves bounded format validation, total referential integrity across
the extended entity set, v1/v2/v3 migration to v4, fail-closed placeholder
synthesis, non-lossy legacy-export behavior, and local warm-open behavior
at the declared v4 entity counts.

It does not prove:

- cold-cache open behavior on declared reference hardware;
- a same-toolchain performance comparison against the v3 workload;
- that a real VST3/CLAP plugin restores from a persisted state blob — the
  blob is stored and returned intact, but no plugin has consumed one;
- that a real media file relinks from a stored path and content hash — no
  relink or media-scan pass exists yet;
- that a stored device configuration opens the named hardware — no backend
  consumes the record yet;
- launcher scenes, tempo maps, or undo history inside the document;
- Java-to-C++ snapshot construction and IPC cost for the new state;
- storage behavior under low space, antivirus contention, network
  filesystems, or device failure;
- a production database/storage-format decision.
