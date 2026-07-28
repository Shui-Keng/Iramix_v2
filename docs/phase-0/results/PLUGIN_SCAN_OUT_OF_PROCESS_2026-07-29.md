# Out-of-Process Plugin Scan — 2026-07-29

Status: P0-013 fourth slice. The first evidence in this task against
**real third-party plugins** rather than a stand-in child. Every previous
P0-013 result carried "nothing about a real plugin" in its evidence
boundary; this one does not.

## Scope

Week 7 asks to "scan a minimal CLAP and VST3 set out of process". The set
here is not minimal: **201 plugin modules already installed on the
development machine**, from roughly 60 vendors, none of them written for
or aware of Iramix.

The property under test is the one that decides whether a DAW starts:
loading an untrusted third-party binary must not be able to take the host
down, hang it, or make it lie about what it found.

Discovery and loading are deliberately separated:

- `discoverPlugins()` decides what *looks* like a plugin from filesystem
  layout alone and loads nothing. It takes its search roots as an
  argument, for the same reason `DeviceResolver` takes an injected
  inventory — the layout rules stay testable on a machine with no plugins
  installed, which includes every CI runner.
- `scanCandidate()` loads exactly one module, in a **separate process**,
  under a per-module timeout.

## Local result

Toolchain: GCC 15.2.0 (MSYS2 UCRT64), Ninja, Debug. Hardware: AMD Athlon
Silver 3050U, 2 cores. Windows-only per accepted risk R-13. VST3 SDK
3.8.0 supplied through `IRAMIX_VST3_SDK_PATH`.

```text
Plugin scan: candidates=201, vst3=198, clap=3, scanned=191, named=188,
not_a_plugin=0, load_failed=7, crashed=0, timed_out=3, host_survived=1,
worst_module_ms=10000, total_scan_ms=92676.9
```

```text
Plugin discovery: roots=3, candidates=3, bundles=1, clap=1,
foreign_bundle_skipped=1, non_modules_skipped=2

Plugin scan rejection: garbage_module=1, host_survived=1,
metadata_invented=0
```

The counts reconcile exactly: 198 VST3 minus 7 that failed to load minus
3 that timed out leaves **188 named**, and the 3 CLAP modules load but are
not named (see below). 188 + 3 = 191 scanned.

Metadata is real, read through the SDK's own declarations:

```text
scanned  AGL.vst3     classes=3  name=Ample Guitar L  vendor=Ample Sound
scanned  Vital.vst3   classes=2  name=Vital           vendor=Vital Audio
scanned  st1b.vst3    classes=1  name=ST1b            vendor=LHI Audio
```

## What the corpus showed

**Nothing crashed the scanner.** `crashed=0` across 201 real binaries, and
the host survived every one. That is a weaker claim than it sounds — it
means no plugin in *this* collection crashes on load, not that none can.
The crash path is exercised separately by the synthetic rejection test,
which confirms a garbage module is refused in ~48 ms and contributes no
metadata.

**Three plugins never finish loading.** Guitar Rig 6 and both Synthesizer V
Studio 2 modules consume the entire 10-second budget every run. They are
not broken — they are presumably doing licence or network work in their
load path. This is the single most important finding for a DAW's startup:
**a small number of legitimate, popular plugins will hang a scan**, and
without a per-module timeout in a separate process they would hang the
application instead.

**Seven fail to load outright**, all from two vendors (Slate Digital's VBC
family and Virtual Mix Rack, plus StandardCLIP). The cause is not
diagnosed here — likely a licensing component or a missing dependency —
but the scanner's job is to record them and move on, which it does.

**A full scan costs ~93 seconds** for 201 plugins, and 30 of those seconds
are the three timeouts. Two runs measured 92.7 s and 128.8 s; load time
varies with disk cache and whatever licensing work each plugin does.
Anything approaching a minute and a half of startup is not acceptable, so
a persistent scan cache keyed on path, size, and mtime is a Phase 1
requirement rather than an optimisation.

## The error-mode trap

The first implementation passed `CREATE_DEFAULT_ERROR_MODE` when spawning
the scan child, with a comment explaining that a plugin must not be able
to pop a dialog. The flag does the opposite of what the comment intended:
it gives the child the *system default* error mode, which **enables** the
modal "bad image" box.

The result was that a malformed module raised a dialog nobody would ever
click, and the scan burned its full 10-second timeout on it —
indistinguishable, from the outside, from a plugin that hangs. It was
caught because a synthetic garbage file timed out instead of failing.

The child now calls `SetErrorMode(SEM_FAILCRITICALERRORS |
SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX)` before loading anything,
and the same garbage module is refused in 48 ms.

This matters beyond the flag: **on Windows, "the plugin hung" and "the
plugin raised a dialog" look identical to the host.** A scanner that only
measures elapsed time will misclassify one as the other.

## Why the VST3 SDK is external

`IRAMIX_VST3_SDK_PATH` follows the existing `IRAMIX_ASIO_SDK_PATH`
pattern, but for a different reason. ASIO is external because its licence
required it. VST3 is MIT since 3.8 and *could* be vendored; it is external
because CI does not need a large third-party tree in order to prove scan
isolation, and the scanner still runs without it — it simply cannot name a
plugin. Only metadata decoding is conditional.

The SDK headers are included as `SYSTEM` so the SDK's own warnings are not
this project's to fix; the build stays clean at `-Wall -Wextra` without
lowering the bar for code Iramix owns.

**The MIT claim is now verified from the SDK itself**, not from the press
release: `vst3sdk/LICENSE.txt` is the MIT text, copyright 2025 Steinberg
Media Technologies GmbH, and `kVstVersionString` reads `VST 3.8.0`. This
closes the confirmation that `DEPENDENCIES.md` asked for on adoption.

## Evidence boundary

This proves out-of-process discovery and load isolation over a large real
corpus on Windows, with per-module timeouts, honest failure classification,
and exact VST3 metadata.

It does not prove:

- **anything about instantiating or running a plugin.** Modules are loaded
  and their factories queried. No plugin is instantiated, no audio is
  processed through one, no editor is opened. The bridge from the earlier
  slices has *not* been connected to a real plugin;
- **anything about CLAP metadata.** The three CLAP modules load and export
  `clap_entry`, which is all that is checked. Decoding a descriptor
  requires the CLAP headers to pin the struct layout, and this project does
  not have them. Reading the fields from a remembered layout would report
  plausible garbage rather than fail, which is worse than reporting
  nothing;
- **that no plugin can crash a scan.** `crashed=0` says nothing in this
  collection crashes on load. The crash-handling path is verified only
  against a synthetic garbage module;
- **why the seven failures fail.** They are recorded, not diagnosed;
- **anything about macOS or Linux.** The bundle layout differs per platform
  and only the Windows path has been exercised against real plugins
  (R-13). The discovery unit test does cover the other layouts, but against
  a synthetic tree;
- **that the timeout budget is right.** Ten seconds was chosen, not
  derived. Three plugins exceed it; whether any legitimate plugin needs
  more is unmeasured;
- **anything about scan caching, parallel scanning, or startup cost in a
  real application.** The ~93-second figure is a single-threaded cold walk
  with no cache.
