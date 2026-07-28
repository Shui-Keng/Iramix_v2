# Persistence and Recovery Foundation — 2026-07-28

Status: initial P0-012 foundation verified; Week 6 remains open.

## Scope

This slice implements three JUCE-free C++20 persistence primitives:

1. a checksummed project snapshot written through a durable staging file and
   atomic replacement;
2. a strictly sequenced, append-only, per-record-checksummed command journal;
3. a recoverable interleaved-float recording staging format with sequenced,
   per-block CRC32.

All formats have independent version fields. Persistence code is control or
worker-thread code and is not called by the real-time audio callback.

## Failure drills

The automated test performs:

- two injected failures after the project staging file has been durably
  flushed but before replacement;
- verification that a failed replacement preserves the committed revision;
- promotion of a valid staging snapshot when the target is missing;
- append of a seven-byte partial journal record, recovery of the valid prefix,
  truncation, and a subsequent valid append;
- launch of a native child process which flushes two recording blocks, writes
  a partial third header, then exits immediately with code 77;
- corruption of one byte in a complete recording block to verify CRC rejection
  of that block and its suffix.

The child uses `CreateProcessW` on Windows and `fork`/`exec` on POSIX. It does
not rely on shell quoting or a graceful application shutdown.

## Windows results

Both GCC/Ninja and MSVC passed all five CTest targets. Raw MSVC persistence
result:

```text
Atomic project store: committed_revisions=2, injected_failures=2,
staging_recoveries=1
Command journal: commands=3, repaired_tails=1,
two_command_durable_ms=6
Recoverable recording: forced_exit=77, flushed_blocks=2,
recovered_frames=4, partial_tails_discarded=1,
corrupt_blocks_rejected=1
All Iramix persistence tests passed.
```

The two durable journal appends completed in 6 ms in this run, below the
five-second acknowledged-edit-loss budget. This is a small-record correctness
screening, not a storage-performance distribution.

## CI and sanitizer results

Source commit: `8f7eaf25519eed4ff18d798b2f5d3ab285859ee3`  
GitHub Actions:
[`30337019452`](https://github.com/Shui-Keng/Iramix_v2/actions/runs/30337019452)

The complete build and test suite passed on Windows, macOS, and Ubuntu.
ASan/UBSan and TSan also passed all five tests without diagnostics.

ASan/UBSan raw persistence result:

```text
Atomic project store: committed_revisions=2, injected_failures=2,
staging_recoveries=1
Command journal: commands=3, repaired_tails=1,
two_command_durable_ms=1
Recoverable recording: forced_exit=77, flushed_blocks=2,
recovered_frames=4, partial_tails_discarded=1,
corrupt_blocks_rejected=1
```

TSan produced the same correctness counters with a reported two-command
durability time of 0 ms at millisecond resolution.

## Evidence boundary

This proves the initial atomicity, ordering, checksum, and forced-process-exit
contracts. It does not yet prove the full Week 6 exit gate:

- recovery currently materializes test audio in memory rather than streaming
  large recordings;
- the current test helper is not final WAV/CAF media conversion;
- snapshot save is synchronous and has not yet been moved behind a worker
  boundary;
- the below-16-ms UI-stall budget is therefore not claimed;
- reference-large-project open below five seconds is not measured;
- disk read-ahead and recording queue pressure are not implemented;
- migration and full session-model round-trip tests remain pending.

Next work is a bounded read-ahead/recording worker with streaming large-file
scan and repair, followed by worker-thread project save and reference-project
benchmarks.
