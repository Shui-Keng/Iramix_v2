# Plugin Process Isolation — 2026-07-28

Status: P0-013 first slice. The bridge, its fail-safe contract, and bridge
overhead are measured on Windows. Plugin scanning, editor embedding, and
state restoration are untouched. Week 7 remains open.

## Scope

This addresses **R-02** (plugin crash or hang destabilizes audio), the
highest-priority risk in the register with no evidence against it.

The slice deliberately carries **no plugin SDK**. CLAP and VST3 remain
unapproved candidates in `DEPENDENCIES.md`, and adopting one would have
mixed a dependency decision into an architectural spike. What is validated
here is the property that has to hold whatever plugin format sits on the
far side: *nothing the child process does may block the audio callback or
take the host down with it.*

`PluginBridge` runs audio through a separate process over shared memory:

- one shared region: a lock-free control block plus input and output audio;
- the host publishes a block, signals the child, then waits **bounded** by
  a configured deadline;
- on expiry the destination is filled with **silence**, the block is
  counted, and the callback returns. It never waits again for that block;
- `processBlock()` allocates nothing, takes no lock, and makes no
  filesystem or logging call.

The child's DSP is a deterministic halving of every sample, so a test can
prove audio actually crossed the process boundary rather than the
destination merely being left untouched.

## Local Release result

Toolchain: GCC 15.2.0 (MSYS2 UCRT64), Ninja, `CMAKE_BUILD_TYPE=Release`.
Hardware: AMD Athlon Silver 3050U, **2 cores / 2 logical processors**.
Windows-only per accepted risk R-13.

```text
Plugin bridge healthy: blocks=500, frames=256, channels=2,
round_trip_p50_ms=0.0139, round_trip_p95_ms=0.0148,
round_trip_p99_ms=0.0191, round_trip_max_ms=0.4049, deadline_misses=0

Plugin process termination: blocks=200, processed=1, degraded=199,
worst_block_ms=6.4877, deadline_misses=203, host_survived=1

Plugin process hang: blocks=50, degraded=49, worst_block_ms=13.9243,
consecutive_misses=49, terminated_on_shutdown=1

Plugin bridge rejections: invalid_configurations=2, unstarted=1,
oversized_blocks=1, double_starts=1, post_stop=1
```

**Bridge overhead** is the healthy round trip: p50 13.9 µs, p99 19.1 µs
for a 256-frame stereo block. Against that block's 5.33 ms deadline at
48 kHz, the p99 crossing costs roughly 0.36% of the budget.

**The host survives both failure modes.** A child that calls `_Exit` mid
stream and a child that stops responding entirely both degrade to counted
silence; the host process continues and the suite runs to completion.

## The design defect this found

The first implementation had the child poll the request sequence with
`std::this_thread::yield()` while the host spun on completion. On this
two-core machine that dropped **116 of 500 blocks (23%)** past a 5 ms
deadline on the healthy path: both sides were spinning, and there are only
two cores for them plus everything else on the machine.

The child now **blocks on a semaphore** (a Windows auto-reset event, a
POSIX named semaphore) and costs nothing while idle. The same test then
processed **500 of 500**. Only the host spins, and only up to the deadline.

This is worth recording because it would not have appeared on a machine
with cores to spare: a bridge that looks correct on a development
workstation can drop a quarter of its blocks on a modest laptop.

## Orphan safety

A plugin process must never outlive its host. The main loop blocks on a
semaphore that a dead host can no longer post, so orphan detection runs on
a separate watchdog thread in the child, checking the parent once a second
and exiting if it is gone.

Verified by killing the host mid-run: peak process count 2 (host plus
child), and zero surviving children three seconds after the host was
killed. Without this a crashed host leaves a blocked child behind, which
in CI is an indefinitely hung job rather than a failed one.

## CI and sanitizer results

GitHub Actions:
[`30365023302`](https://github.com/Shui-Keng/Iramix_v2/actions/runs/30365023302)

All five jobs passed. ASan/UBSan and TSan matter more than usual here: the
bridge maps atomics into a region two processes write, forks and execs a
child, and detaches a watchdog thread. A race in the sequence protocol
would surface in TSan rather than as an occasional dropped block.

One macOS-only failure was fixed on the way. macOS caps POSIX
shared-memory and semaphore names at 31 characters including the leading
slash; the semaphore name was 33, so `sem_open` failed there while Linux
and Windows, which have no such limit, passed. macOS reports nothing more
specific than a failed open. Both names are now derived from a short hex
token, and the child derives them the same way from the token it is given
rather than being handed a pre-built path.

This is the third failure this project has hit of the same shape — a
construct that compiles and runs on the one local toolchain and fails
elsewhere, after `std::uintmax_t` on macOS and Windows SDK header order on
MSVC. The pattern is recorded in `CLAUDE.md`.

## Evidence boundary

This proves the bridge contract — bounded wait, silence on failure, host
survival across child crash and child hang — plus measured bridge overhead
and orphan reaping.

It does not prove:

- **anything about a real plugin.** No CLAP, VST3, or Audio Unit is
  scanned, loaded, or hosted. The child is a stand-in with deterministic
  DSP, and the two failure modes are injected rather than observed in the
  wild;
- **that the deadline bounds the wait under preemption.** It bounds the
  *spin*, which only advances while the thread is scheduled. The worst
  observed block was 13.9 ms against a 5 ms deadline — roughly 2.8× — on a
  loaded two-core machine. A real-time audio thread with priority would do
  better, and the probe's MMCSS path is not used here;
- that spinning on the host side is acceptable in production. It burns a
  core for up to one deadline per block. A production bridge should use a
  timed futex-style wait; `sem_timedwait` is unavailable on macOS, which is
  why the portable spin was chosen for this spike;
- any behaviour under sanitizers beyond what CI reports, or on macOS and
  Linux hardware (R-13);
- plugin **state restoration**, moved here from P0-012 and still
  unimplemented: no state blob has been handed to a hosted plugin;
- editor embedding constraints, shared-memory transport for control and
  parameter traffic, or bridge behaviour with more than one plugin
  process.
