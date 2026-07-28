# Bounded Disk-Audio Workers — 2026-07-28

Status: P0-012 worker slice verified locally and on hosted three-OS CI plus
sanitizers; Week 6 remains open.

## Scope

This slice adds three JUCE-free C++20 production boundaries:

1. a streaming recording scanner/reader and invalid-tail repair path;
2. a callback-to-disk recording worker with a preallocated SPSC block queue;
3. a disk-to-callback playback read-ahead worker with a preallocated SPSC
   block queue.

The callback-facing recording operation copies one interleaved block into an
available slot and publishes an atomic index. When full, it rejects the block
and increments a counter. Playback copies a ready slot into the destination.
When empty, it writes deterministic silence and increments an underflow
counter. Neither path opens files, computes CRCs, flushes storage, waits, or
uses a mutex.

The workers own file reads/writes, per-block checksum validation, and durable
flush cadence. Queues and worker scratch space are allocated before the worker
or callback starts.

## Method

The persistence test executable now links the same temporary global
`operator new/delete` hooks used by the audio probe. Enqueue, dequeue, and
underflow calls run inside `realtime::CallbackScope`; the test asserts the
allocation, deallocation, and tracked-blocking-lock counters are all zero.

Queue pressure is deterministic:

- a two-slot recording queue is filled before its worker starts;
- the third block must be rejected and counted;
- the worker then drains and durably flushes both accepted blocks;
- read-ahead is called before start to force a silence underflow;
- after prefill, two blocks must arrive in order;
- a third dequeue must again produce silence and count the underflow.

Streaming recovery is screened with 2,048 blocks of 256 stereo frames. The
4,243,472-byte file is scanned with a fixed 65,536-byte scratch array and no
sample materialization. A separate forced-exit file verifies a seven-byte
partial tail is detected and truncated from 103 bytes to the 96-byte valid
prefix.

## Local results

Source commit: `fd2f9bbbfc54778594ea4800c9391bb7b94c257f`

GCC/Ninja and MSVC both built without errors and passed all five CTest targets
on Windows. Raw MSVC persistence output:

```text
Atomic project store: committed_revisions=2, injected_failures=2,
staging_recoveries=1
Command journal: commands=3, repaired_tails=1,
two_command_durable_ms=6
Recoverable recording: forced_exit=77, flushed_blocks=2,
recovered_frames=4, partial_tails_discarded=1,
corrupt_blocks_rejected=1, stream_scan_buffer_bytes=65536,
repaired_bytes=7
Recording worker scan: ok=1, blocks=2, frames=4,
valid_bytes=96, file_bytes=96, invalid_tail=0, error=
Disk audio workers: recording_queue_blocks=2, recording_rejected=1,
recording_written=2, read_ahead_blocks=2, playback_underflows=2,
queue_bytes_each=40, callback_allocations=0,
callback_blocking_locks=0
Bounded streaming scan: file_bytes=4243472, blocks=2048,
frames=524288, scratch_bytes=65536, materialized_samples=0
All Iramix persistence tests passed.
```

The 65,536-byte number is the scanner's fixed implementation capacity, not a
whole-process peak-RSS measurement. Queue bytes cover sample slots and frame
counts; worker control objects and thread stacks are separate bounded
overhead.

## CI and sanitizer results

GitHub Actions:
[`30338491614`](https://github.com/Shui-Keng/Iramix_v2/actions/runs/30338491614)

The complete suite passed on Windows, macOS, and Ubuntu. ASan/UBSan and TSan
also passed all five tests without diagnostics. Hosted CI establishes build,
format/recovery behavior, and sanitizer coverage for these operating systems;
it does not exercise a real audio endpoint or reference storage device.

## Evidence boundary

This proves deterministic queue capacity, pressure reporting, sample ordering,
silence-on-underflow, callback allocation/lock instrumentation, streaming
checksum validation, and suffix-only repair for the test corpus.

It does not prove:

- dropout behavior on a real audio device;
- read-ahead sufficiency under slow or contended storage;
- full-scale multi-hour or multi-gigabyte recording performance;
- live session/device integration;
- final WAV/CAF conversion;
- asynchronous project save and the below-16-ms UI-stall target;
- project migrations, complete session round trips, or the below-five-second
  reference-project open target.

Hosted Windows/macOS/Linux builds and sanitizers validate portability and
memory/thread safety only; they are not audio-hardware or storage-performance
evidence.
