# Dependency and License Inventory

This document separates **what the build actually consumes today** from
**candidates under consideration**. Appearing in the candidate table
approves nothing.

Every production dependency requires a pinned version, a license record, a
security owner, an update policy, and three-OS build proof.

The Java table is machine-regenerable:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\license-inventory.ps1
```

It reads a Gradle lockfile, resolves each locked coordinate to its POM in
the Gradle module cache, and exits non-zero if any artifact declares no
license or is absent from the cache — so a new dependency cannot enter
unnoticed.

## In the build today

### C++ engine (`iramix_core`)

**Zero third-party dependencies.** The engine links only the C++20
standard library. Nothing else is vendored, fetched, or found at configure
time. `third_party/` does not exist.

This is the position to defend: it is what keeps license, supply-chain,
and real-time risk out of the audio path entirely.

### Java UI runtime

Nine locked artifacts, all declaring Apache-2.0. Verified from the POMs in
the Gradle module cache on 2026-07-28:

| Coordinate | Declared license |
|---|---|
| `org.jetbrains.kotlin:kotlin-stdlib:2.3.20` | Apache-2.0 |
| `org.jetbrains.kotlinx:kotlinx-coroutines-bom:1.8.0` | Apache-2.0 |
| `org.jetbrains.kotlinx:kotlinx-coroutines-core:1.8.0` | Apache-2.0 |
| `org.jetbrains.kotlinx:kotlinx-coroutines-core-jvm:1.8.0` | Apache-2.0 |
| `org.jetbrains.runtime:jbr-api:1.5.0` | Apache-2.0 |
| `org.jetbrains.skiko:skiko-awt:0.150.1` | Apache-2.0 |
| `org.jetbrains.skiko:skiko-awt-runtime-<target>:0.150.1` | Apache-2.0 |
| `org.jetbrains:annotations:13.0` | Apache-2.0 |
| `org.jetbrains:annotations:23.0.0` | Apache-2.0 |

The set is identical across all five target lockfiles apart from the
platform-specific Skiko runtime artifact.

### Toolchains

| Component | Pin | License | Notes |
|---|---|---|---|
| Eclipse Temurin JDK | 21.0.11+10, SHA-256 pinned in `toolchains.lock.json` | GPL-2.0 **with Classpath Exception** | The Classpath Exception is what permits shipping a runtime image alongside closed-source application code. Record it deliberately; without it this pin would be a blocker. |
| Gradle | 9.6.1, SHA-256 pinned | Apache-2.0 | Build-time only, not redistributed |
| CMake + Ninja | Not pinned | BSD-3-Clause / Apache-2.0 | Build-time only, not redistributed |
| MSVC / AppleClang / GCC | Not pinned | Vendor terms | Build-time only |

### Platform libraries (audio probe only)

| Platform | Linked | Provenance | Redistribution |
|---|---|---|---|
| Windows | `avrt`, `ksuser`, `ole32` | Windows SDK import libraries | OS components; not redistributed |
| macOS | `CoreAudio` framework | Apple SDK | OS component; not redistributed |
| Linux | `libjack` via `pkg-config` | Distribution package | **See obligation L-2 below** |

None of these are linked into `iramix_core`; they reach only the
`iramix_audio_probe` executable.

### CI supply chain

| Action | Pin |
|---|---|
| `actions/checkout` | `@v7` |
| `actions/setup-java` | `@v5` |
| `gradle/actions/setup-gradle` | `@v6` |

**See obligation L-3 below.**

## Open obligations

These are the findings this inventory exists to produce. None is
theoretical; each names a concrete action.

### L-1 — Skiko runtime jars ship unattributed native code

`skiko-awt-runtime-windows-x64-0.150.1.jar` contains exactly five entries:
a manifest, `skiko-windows-x64.dll` (14,109,696 bytes), its checksum, and
`icudtl.dat` (10,468,208 bytes). **There is no `LICENSE`, `NOTICE`, or
third-party attribution file anywhere in the jar.**

The POM's Apache-2.0 covers Skiko's own source. It does not describe what
the DLL statically links.

**Updated 2026-07-29 — the obligation is larger than this entry assumed.**
The DLL was inspected directly rather than reasoned about. It carries
compiled-in copyright notices for **libpng, libjpeg-turbo, and zlib**,
a version marker for **Expat 2.7.4**, and name-level references to
**HarfBuzz, libwebp, Wuffs, and ANGLE** — none of which this entry had
listed. `icudtl.dat`'s header carries its own ICU notice verbatim:

```text
Copyright (C) 2016 and later: Unicode, Inc. and others.
License & terms of use: http://www.unicode.org/copyright.html
```

So the count went from two assumed components to at least five evidenced
by embedded copyright or version strings, plus four more by name. The
original entry's "Skia and, on the evidence of `icudtl.dat`, ICU" was an
undercount, and shipping an attribution file built on it would have been
non-compliant in a way nobody would have noticed.

[`NOTICE.md`](../../NOTICE.md) now carries what was verified, with the
verbatim attribution strings and an evidence grade per component.

Action, narrowed by that finding: obtain the upstream third-party notice
set for the pinned Skiko version and reconcile it against `NOTICE.md`.
The file is a floor, not a discharge — **L-1 stays open**, because:

- only `windows-x64` was inspected; the other four target jars resolve to
  POMs until a build selects them, and Linux/macOS may link components
  Windows does not (FreeType is absent from the Windows library and is
  the obvious candidate elsewhere);
- string scanning cannot prove absence, so a component that leaves no
  string would not appear at all;
- licence *versions* in `NOTICE.md` are as upstream publishes them today,
  not as they stood when Skiko 0.150.1 was built.

### L-2 — JACK client library licence is unverified from here

`libjack` is linked on Linux via `pkg-config`. The upstream project
licenses the client library under LGPL, with the server under GPL, which
would make dynamic linking acceptable and static linking a policy
violation under "no GPL dependency in a closed-source path".

**This has not been verified from this machine** — the project has no
Linux hardware (R-13), and the claim above is from upstream reputation, not
from an inspected package.

Action: read `/usr/share/doc/libjack-jackd2-dev/copyright` on the CI
Ubuntu runner and record the actual SPDX identifiers, then confirm the
link is dynamic. Until then, treat Linux JACK as spike-only, which it
currently is.

**Instrumented 2026-07-29; still open pending the first log.** The Ubuntu
CI job gained a "Record JACK licence and link type" step that prints the
installed package versions, the `License:` lines from the Debian
copyright file, `pkg-config --libs jack`, and which library files the
packages ship (`.so` versus `.a`). The step **prints rather than
asserts**: an upstream relicence should surface as a visible diff in the
log, not as a red build that no one here can reproduce without a Linux
machine (R-13).

This closes the "cannot be verified from this machine" half. It does not
close L-2 — that needs a human to read the first log and paste the actual
SPDX identifiers and link type into this entry, replacing the upstream
reputation the paragraph above still rests on.

### L-3 — CI actions are pinned to mutable tags

`@v7`, `@v5`, and `@v6` are branch-like major tags that upstream can
repoint at any time. A compromised or merely changed action would execute
with repository credentials, and nothing in this repository would record
that the code had changed.

Action: pin each action to a full commit SHA with the version in a
trailing comment. This costs nothing and is the standard hardening for
third-party actions.

**Closed 2026-07-29.** All four `uses:` entries in
`.github/workflows/ci.yml` are pinned:

| Action | Commit SHA | Version at pinning |
|---|---|---|
| `actions/checkout` | `3d3c42e5aac5ba805825da76410c181273ba90b1` | v7.0.1 |
| `actions/setup-java` | `03ad4de0992f5dab5e18fcb136590ce7c4a0ac95` | v5.6.0 |
| `gradle/actions/setup-gradle` | `3f131e8634966bd73d06cc69884922b02e6faf92` | v6.2.0 |

Each SHA was resolved from the major tag and then confirmed identical to
the exact version tag, so the trailing comments describe the pinned
commit rather than merely the tag that pointed at it. Updating an action
is now a visible diff with a reviewable SHA change.

### L-4 — Two `org.jetbrains:annotations` versions coexist

`13.0` on the compile classpath and `23.0.0` at runtime. Same license, no
legal issue, but a version skew worth resolving before it becomes a
behavioural surprise.

## Candidates — not approved, not in the build

| Area | Candidate | Purpose | Decision |
|---|---|---|---|
| Audio files | To be selected | WAV/AIFF/FLAC read/write | Open |
| Sample-rate conversion | To be selected | Boundary conversion | Open |
| FFT | To be selected | Spectral analysis and DSP | Open |
| Production storage | SQLite plus schema layer | Session persistence | Open |
| Windows audio | Steinberg ASIO SDK 2.3 | Optional external probe | Proprietary selected; signing pending |
| Plugin API | CLAP | Plugin hosting | Planned; MIT upstream, to be confirmed on adoption |
| Plugin API | VST3 SDK 3.8 or later | Plugin hosting | Planned; **MIT since VST 3.8 (2025-10-29)**. No agreement required, no disclosure obligation. Pin ≥ 3.8 — earlier releases are dual GPLv3/proprietary |
| Plugin API | Apple Audio Unit | macOS plugin hosting | Planned; Apple SDK terms |

### Resolved since the previous revision

- **Unit test framework** — no longer open. The project uses a
  hand-written harness (`tests/*.cpp`, a `main()` per suite with a
  `require()` helper) and takes on no test-framework dependency.
- **Session serialization** — the deterministic binary schema is
  implemented and now at v4. SQLite remains a production candidate for
  storage, not for the document format.

## Dependency policy

- No GPL dependency may enter a closed-source product path without
  explicit licensing and legal approval.
- Audio callback code may not call a dependency unless its real-time
  behaviour is understood and tested.
- Dependencies using a different build system are wrapped as immutable,
  reproducible artifacts.
- Every dependency receives automated license and vulnerability inventory.
  License inventory is automated by `scripts/license-inventory.ps1`;
  **vulnerability inventory is not yet automated.**
- Updating Skiko/Skia or a plugin SDK requires compatibility tests, not
  only a successful build.

## VST3 licence record

- Steinberg released the **VST 3.8 SDK under the MIT licence on
  2025-10-29**, replacing the previous dual GPLv3/proprietary model. MIT
  permits use in a closed-source commercial product provided the copyright
  and licence text are retained, so **no developer agreement and no source
  disclosure is required** — unlike ASIO.
- This removes what was previously recorded here as a blocking decision
  for VST3 plugin hosting. That entry was stale: it described the terms
  before 3.8.
- **The version matters.** Releases before 3.8 remain under the old dual
  model, so the dependency must be pinned to 3.8 or later. Adopting an
  older tag would silently reintroduce the GPLv3 obligation.
- **Confirmed against the SDK itself on 2026-07-29**, as this entry
  previously required. `vst3sdk/LICENSE.txt` is the MIT text, copyright
  2025 Steinberg Media Technologies GmbH, and
  `pluginterfaces/vst/vsttypes.h` declares `kVstVersionString` as
  `VST 3.8.0`. The press release of 2025-10-29 was the announcement; this
  is the licence.
- The SDK is **not vendored**. `IRAMIX_VST3_SDK_PATH` points at a
  separately supplied checkout, following the ASIO pattern but for a
  different reason: the licence permits vendoring, and the tree is kept
  external only because CI does not need it. Headers are included as
  `SYSTEM` so the SDK's warnings are not this project's to fix.
- Only plugin *metadata* decoding depends on the SDK. The out-of-process
  scanner builds and runs without it; see
  `results/PLUGIN_SCAN_OUT_OF_PROCESS_2026-07-29.md`.
- The VST name and logo are trademarks governed separately from the SDK
  licence, exactly as with ASIO. The same default policy applies: protocol
  support without branding until branding is deliberately approved.
- Primary reference: Steinberg press release, "VST 3 Now Available Under
  MIT License", 2025-10-29.

## ASIO SDK spike record

- The SDK is not vendored in this repository. The optional
  `IRAMIX_ASIO_SDK_PATH` CMake cache path must point to a separately
  supplied checkout.
- Steinberg has offered ASIO under a dual proprietary/GPLv3 model since
  October 2025. Iramix has selected the free proprietary license because
  the planned commercial product is closed-source; this path has no
  source-code disclosure obligation. GPLv3 is explicitly not the selected
  path.
- Official distribution requires an ASIO developer license agreement with
  Steinberg through the Developer Portal. That agreement has not been
  signed, so builds containing the ASIO SDK remain
  experimental/development-only until signing is complete.
- Technical work and Phase 0 measurement may continue while signing is
  pending. The signing task gates only a distributable public release
  build.
- Use of the ASIO name or logo is optional but governed separately by
  Steinberg's trademark and usage rules. The default Iramix policy is
  silent protocol support without ASIO branding or logo until branding is
  deliberately approved.
- Primary references:
  [Steinberg dual-license announcement](https://ocl-steinberg-live.steinberg.net/_storage/asset/808575/storage/master/Press%20Release%20-%202025-10-15%20-%20OBS%20Partnership-%20EN.pdf)
  and [Steinberg Developer Portal](https://www.steinberg.net/developers/).
