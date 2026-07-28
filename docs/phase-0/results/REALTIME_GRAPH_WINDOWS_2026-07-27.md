# Immutable Real-time Graph Screening — Windows — 2026-07-27

## Scope

This is initial evidence for the JUCE-free Tier-1 graph core. It is not final
acceptance of ADR-0003 and it is not a hardware callback or three-OS result.

The implementation under test contains:

- a non-owning planar `float` buffer ABI;
- asymmetric multi-bus prepare metadata, including sidechain roles;
- an explicit per-block transport snapshot;
- fixed-capacity, sample-sorted MIDI storage;
- control-thread graph compilation with cycle and port validation;
- deterministic fan-in, buffer pooling, and graph-level plugin delay
  compensation;
- a prepared executor with atomic plan publication;
- acknowledgement-based, control-thread reclamation;
- prepared-plan ownership of node lifetimes.

## Environment

- OS: Windows
- MSVC: Visual Studio 2022 Build Tools, MSVC 14.44
- Secondary compiler: GCC 15.2.0 through MSYS2 UCRT64/Ninja
- Configuration exercised: Debug
- Test targets: `iramix_audio_graph_tests`, `iramix_plan_swap_tests`

## Functional result

The deterministic graph fixture used two source paths feeding one summing
node. One path reported and produced three samples of latency. The compiler
inserted one three-sample compensation line on the faster path.

Raw output:

```text
Graph render audit: allocations=0, deallocations=0, blocking_locks=0, pdc_peak_frame=3, pdc_peak=2
All Iramix audio graph tests passed.
```

The same fixture verified:

- both impulses aligned at frame 3 and summed to exactly `2.0`;
- all other output frames were zero;
- MIDI events from two producers arrived sorted at offsets 4 and 12;
- sample position `96000` and tempo `137.5` reached the processing node;
- a two-node cycle was rejected;
- an out-of-range audio connection was rejected;
- a 10-input/2-output prepare layout remained representable.

## Concurrent publication result

One dedicated thread repeatedly rendered 64-frame blocks while the control
thread published 1,000 replacements after the initial plan.

Raw output from the recorded MSVC run:

```text
Plan swap stress: publications=1001, blocks=453154, observed_swaps=896, reclaimed=1000, retired=0, allocations=0, deallocations=0, blocking_locks=0, use_after_free=0
```

The allocation figures come from global `operator new`/`operator delete`
hooks combined with the callback thread-local audit scope. Blocking-lock
attempts are counted by the tracked-lock hook. Node objects are retained by
the prepared plan and destroyed only when the control thread reclaims that
plan.

## Build and test result

Both compiler paths completed all current C++ tests:

```text
100% tests passed, 0 tests failed out of 4
```

The GCC/Ninja check also exposed a pre-existing missing Windows `ksuser`
link dependency in the WASAPI probe. The CMake target now links `ksuser`,
and the full GCC/Ninja build completes.

## What remains open

- The executor is now connected to the WASAPI render callback and the initial
  device-input, track, gain, mixer, and output nodes are implemented. See
  [`WASAPI_GRAPH_INTEGRATION_WINDOWS_2026-07-27.md`](WASAPI_GRAPH_INTEGRATION_WINDOWS_2026-07-27.md).
- Bounded sample-accurate parameter events and a live WASAPI plan replacement
  are recorded in
  [`WASAPI_GRAPH_CONTROL_WINDOWS_2026-07-28.md`](WASAPI_GRAPH_CONTROL_WINDOWS_2026-07-28.md).
- ASIO, Core Audio, JACK, PipeWire, and ALSA integration remains pending.
- Scalar production-node parameters support sample-offset events; ramps and
  modulation streams remain pending.
- The parameter queue is bounded; general engine commands and audio telemetry
  queues remain pending.
- Queue-saturation and denormal tests are not implemented.
- PDC coverage is an initial graph-level fixture, not yet the full plugin,
  sidechain, bypass, oversampling, and routing-change matrix.
- Sanitizer evidence is still required for repeated plan replacement.
- macOS and Linux build/runtime evidence is still pending.
- A two-hour hardware callback soak remains part of P0-008, not this result.

Therefore this result establishes a usable native graph foundation but does
not close the Week 5 exit gate.
