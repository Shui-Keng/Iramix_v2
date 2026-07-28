# Real VST3 Instantiation and State Round Trip — 2026-07-29

Status: P0-013 fifth slice. Removes the boundary every previous P0-013
result carried: **a real plugin is now instantiated, processes audio, and
round-trips its own state.** The scan slice loaded modules; this one runs
them.

## Scope

`Vst3Host` loads a module, finds its audio class, instantiates
`IComponent`, queries `IAudioProcessor`, activates buses, sets up
processing, and drives blocks. It also saves and restores the plugin's
own opaque state through `IComponent::getState` / `setState`.

Only the SDK's **interface headers** are used. No SDK source is compiled
or linked: `DECLARE_CLASS_IID` defines the interface TUIDs in-header, so
`IBStream` and `IHostApplication` are implemented directly here — about a
hundred lines — rather than pulling in `public.sdk`.

Two host-side objects were unavoidable:

- **`MemoryStream`**, an `IBStream` over a byte vector. State transfer has
  no other shape.
- **`HostApplication`**, a minimal `IHostApplication`. Plugins query the
  host for its identity during `initialize()`, and several refuse to
  start without one. `createInstance` honestly returns `kNotImplemented`
  rather than half-implementing objects Iramix does not yet provide.

## Local result

Toolchain: GCC 15.2.0 (MSYS2 UCRT64), Ninja, Debug. Hardware: AMD Athlon
Silver 3050U, 2 cores. VST3 SDK 3.8.0 via `IRAMIX_VST3_SDK_PATH`.
532 blocks of 256 frames, stereo, 48 kHz. Windows-only per R-13.

```text
Plugin host: name=Diopser, in_channels=2, out_channels=2, open_ms=14.6291,
blocks=532, processed=532, block_p50_ms=0.0089, block_p95_ms=0.009,
block_p99_ms=0.0091, block_max_ms=0.199, input_peak=0.249997,
output_peak=0.249997, samples_changed=0, state_bytes=279, state_saved=1,
state_restored=1, state_stable=1, save_ms=0.046, load_ms=0.0465,
audio_after_restore=1, peak_after_restore=0.249997

Plugin host: name=Crisp, in_channels=2, out_channels=2, open_ms=23.6563,
blocks=532, processed=532, block_p50_ms=0.0114, block_p95_ms=0.0115,
block_p99_ms=0.0558, block_max_ms=1.1509, input_peak=0.249997,
output_peak=0.44973, samples_changed=0.566406, state_bytes=359,
state_saved=1, state_restored=1, state_stable=1, save_ms=1.8487,
load_ms=0.2226, audio_after_restore=1, peak_after_restore=0.452902

Plugin host: name=Crossover, in_channels=2, out_channels=2,
open_ms=22.2718, blocks=532, processed=532, block_p50_ms=0.0051,
block_p99_ms=0.0051, block_max_ms=0.0394, output_peak=0.249997,
samples_changed=0, state_bytes=185, state_saved=1, state_restored=1,
state_stable=1, audio_after_restore=1

Plugin host: name=Vital, in_channels=0, out_channels=2, open_ms=254.553,
blocks=532, processed=532, block_p50_ms=0.0336, block_p95_ms=0.0346,
block_p99_ms=0.048, block_max_ms=9.0005, output_peak=0,
samples_changed=0.996094, state_bytes=174925, state_saved=1,
state_restored=1, state_stable=0, save_ms=4.8371, load_ms=37.2723,
audio_after_restore=1

Plugin host: name=ANINA, in_channels=2, out_channels=2, open_ms=178.119,
blocks=532, processed=532, block_p50_ms=0.1252, block_p95_ms=0.1452,
block_p99_ms=0.1851, block_max_ms=0.4539, input_peak=0.249997,
output_peak=0.139786, samples_changed=1, state_bytes=566, state_saved=1,
state_restored=1, state_stable=1, save_ms=1.2903, load_ms=0.0632,
audio_after_restore=1, peak_after_restore=0.139786
```

**Every plugin processed every block**: 532/532, five for five.

## Why `samples_changed` exists

Peak level proves nothing about whether audio reached a plugin's DSP. An
allpass filter changes a signal without changing its peak, and a host that
silently copied its input to its output would look identical.
`samples_changed` is the fraction of samples that differ from the input by
more than 1e-6.

It also stops the result being over-read in the other direction. Diopser
and Crossover report `samples_changed=0` — **not** because audio bypassed
them, but because both are transparent at their default state: Diopser's
filter spread starts at zero and Crossover's bands sum back to unity.
Crisp (0.57) and ANINA (1.0) are the two that demonstrate audio genuinely
passing through third-party DSP, and ANINA's peak drops from 0.25 to 0.14
while doing it.

## Three findings

**A plugin wrote to the host's stdout.** ANINA is built on Pure Data and
printed its banner — `pd 0.56.2`, `bonk version 1.5`, and five more lines
— straight to the process's standard output during `initialize()`.

This matters specifically to Iramix. P0-007's Java↔C++ boundary is a
**stdio transport**. A plugin hosted in that process would have injected
those lines into the protocol stream. Process isolation was justified
until now by crash and hang safety; this is a third, quieter reason, and
it would have corrupted a channel rather than crashing it.

**A plugin's state is not necessarily byte-stable.** Vital saves 174,925
bytes; restoring that blob and immediately saving again produces
*different* bytes (`state_stable=0`). The other four are stable. Nothing
here diagnoses why — a timestamp, a seed, or map ordering are all
plausible — but the consequence is concrete: **a session must not treat a
re-captured plugin blob as evidence that nothing changed.** A dirty check
built on comparing captured state would mark a Vital track dirty forever.

**Instruments have no input bus.** Vital reports `in_channels=0`, so a
host that assumes a symmetric in/out bus layout fails on every synth. It
also outputs silence here, correctly — it was sent no MIDI.

## Cost

`open_ms` ranges from 14.6 ms (Diopser) to 254.6 ms (Vital), which is
consistent with the scan slice's finding that instantiation cost is
dominated by what a plugin does at startup rather than by the host.

Per-block cost at 256 frames is 5.1 µs to 125 µs at p50 — against a
5.33 ms budget, between 0.1% and 2.4%. Vital's `block_max_ms=9.0` exceeds
the block budget outright on a single block; on a two-core machine under
Debug that is unsurprising and is exactly the class of event the bridge's
bounded deadline exists to absorb.

State transfer is sub-millisecond except for Vital's 37 ms restore of a
171 KiB blob, which is a useful data point for R-12: **restoring plugin
state is not free, and a project with several large synths will pay tens
of milliseconds per plugin at load.**

## Evidence boundary

This proves real VST3 instantiation, block processing, and state round trip
on Windows, against five third-party plugins, with the SDK used only as
interface declarations.

It does not prove:

- **that any of this runs across the bridge.** `Vst3Host` is exercised
  in-process by a probe application. It is *designed* to run inside the
  plugin child, and nothing yet wires it there — the bridge still hosts
  the stand-in. Connecting the two is the remaining step, and until it is
  taken the crash/hang isolation evidence and the real-plugin evidence
  belong to two different programs;
- **anything about parameters.** `IEditController` is not instantiated,
  no parameter is read or written, and the bridge's parameter transport is
  not connected to a real plugin. `samples_changed=0` for two plugins is
  precisely because their *default* state is transparent and nothing here
  can change it;
- **anything about instruments.** Vital is instantiated and produces
  silence because it receives no MIDI. No event input is implemented;
- **anything about latency, tail, or bus reconfiguration.** Only bus 0 in
  and bus 0 out are activated, `getLatencySamples` is not queried, and
  side-chains and multi-out layouts are untouched;
- **that five plugins generalise.** They were chosen because they are
  installed here. Four are small and open-source; one commercial synth.
  The 201-module corpus from the scan slice has *not* been run through
  instantiation;
- **anything about macOS or Linux** (R-13), or under sanitizers — the SDK
  is absent from CI by design, so none of this code path is exercised
  there at all;
- **why Vital's state is unstable.** Observed, not diagnosed.
