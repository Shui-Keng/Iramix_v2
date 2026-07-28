# Media Relink and Restoration — 2026-07-28

Status: P0-012 media restoration verified locally; hosted three-OS CI and
sanitizer confirmation pending. Week 6 remains open.

## Scope

[`COMPLETE_SESSION_STATE`](COMPLETE_SESSION_STATE_2026-07-28.md) stored
media references but nothing consumed them. This adds the first consumer:
a resolver that decides, for each declared media source, whether the file
the document names is present and is actually the same file.

`resolveSessionMedia` is read-only and classifies every source:

| Status | Meaning |
|---|---|
| `verified` | Declared path opened, content hash matched |
| `mismatched` | Declared path opened, bytes differ — a different take |
| `relocated` | Declared path gone, a search directory held a hash match |
| `unverifiable` | A file exists but the source names no hash to check |
| `missing` | No candidate found, including every v3 placeholder |

`applyMediaResolution` rewrites paths for `relocated` sources only.
Mismatched, unverifiable, and missing sources are left byte-for-byte
untouched, so a failed relink can never silently bind a session to the
wrong audio. It returns the number of paths rewritten, and prefers a
project-relative path when the located file sits under the project root so
a relinked project stays portable.

## Identity check

`hashMediaFile` computes an FNV-1a 64 hash over at most
`maximumHashBytes` (default 64 MiB) of file content, then mixes in the
full byte length. The length mix means a file whose first
`maximumHashBytes` are identical but which was extended or truncated past
that bound still fails verification — verified directly rather than
assumed, with an 8-byte bound over two files sharing a prefix.

**This hash is not cryptographic.** It detects substitution, truncation,
and extension. It does not resist a deliberately crafted collision, and
nothing in this design should be read as authenticating media provenance.

Relocation searches each configured directory recursively for a file whose
*filename* matches the declared one, then verifies the hash before
accepting it. A renamed file is therefore not relocated — only a moved
one. The scan is bounded by `maximumSearchEntries` (default 100,000) and
reports `searchBudgetExhausted` when it stops early; a directory that
cannot be read is skipped rather than aborting the whole relink.

## Local Release result

Source commit: `a8bf0eb` (working tree, media resolver applied)

Toolchain: GCC 15.2.0 (MSYS2 UCRT64), Ninja, `CMAKE_BUILD_TYPE=Release`.

```text
Media resolution: sources=6, verified=1, mismatched=1, relocated=1,
unverifiable=1, missing=2, applied=1, search_entries=4,
budget_exhausted=0, bounded_length_separations=1,
replacements_detected=1, unreadable_files_rejected=1
```

The fixture covers one intact file, one replaced in place, one moved into
a separate vault directory, one stored without a hash, one deleted, and
one schema v3 migration placeholder. After applying the single relocation,
re-resolving the same document reports two verified sources and zero
relocations, so a relinked path holds on the next open. The relinked
document still serializes, so relinking cannot produce a document that
fails schema validation.

Full local suite passes:

```text
100% tests passed out of 6
```

## Evidence boundary

This proves classification of present, replaced, moved, unhashed, and
absent media; fail-closed application that only ever rewrites a verified
relocation; bounded and error-tolerant directory scanning; and detection
of content changes past the hash bound.

It does not prove:

- hosted three-OS CI or sanitizer results for the resolver (not yet run);
- that any audio actually decodes from a resolved path — nothing here
  opens a media file as audio, and `SessionMediaSource::frameCount`,
  `sampleRate`, and `channelCount` remain unverified descriptive metadata
  because the codebase has no audio-file decoder;
- relocation of renamed files, or of sources whose hash is unknown;
- behavior against network filesystems, case-insensitive path collisions,
  or symlink loops in a search directory;
- resolver cost at reference media scale — the fixture is six small files,
  and no throughput figure should be inferred from it;
- plugin state restoration or device configuration restoration, which
  remain stored but unconsumed.
