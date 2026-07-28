# Dependency and License Inventory — 2026-07-28

Status: P0-010 inventory produced and automated for the Java dependency
set; four obligations recorded, none yet discharged.

## Scope

[`DEPENDENCIES.md`](../DEPENDENCIES.md) previously mixed dependencies that
are in the build with candidates that are not, and listed licenses without
recording where they came from. This separates the two, records provenance
for every claim, and makes the Java table regenerable rather than a
snapshot a reader has to trust.

## Method

Licenses were **read from the POMs in the local Gradle module cache**, not
recalled or looked up. `scripts/license-inventory.ps1` reads a Gradle
lockfile, resolves each locked coordinate to its cached POM, extracts the
declared license, and exits non-zero if any artifact declares none or is
absent from the cache.

```text
Coordinate                                                License      Source
org.jetbrains.kotlin:kotlin-stdlib:2.3.20                 Apache-2.0   pom
org.jetbrains.kotlinx:kotlinx-coroutines-bom:1.8.0        Apache-2.0   pom
org.jetbrains.kotlinx:kotlinx-coroutines-core:1.8.0       Apache-2.0   pom
org.jetbrains.kotlinx:kotlinx-coroutines-core-jvm:1.8.0   Apache-2.0   pom
org.jetbrains.runtime:jbr-api:1.5.0                       Apache-2.0   pom
org.jetbrains.skiko:skiko-awt:0.150.1                     Apache-2.0   pom
org.jetbrains.skiko:skiko-awt-runtime-windows-x64:0.150.1 Apache-2.0   pom
org.jetbrains:annotations:13.0                            Apache-2.0   pom
org.jetbrains:annotations:23.0.0                          Apache-2.0   pom

license_inventory lockfile=ui/desktop/gradle-windows-x64.lockfile
artifacts=9 undeclared=0
```

The failure path was verified rather than assumed — a lockfile naming an
uncached coordinate reports it and exits 1:

```text
com.example:not-a-real-artifact:9.9.9  UNDECLARED  not-cached
license_inventory ... artifacts=1 undeclared=1
1 artifact(s) declare no license or are absent from the cache.
```

C++ dependencies were established by reading `CMakeLists.txt` directly:
no `find_package` beyond an optional `pkg_check_modules(JACK)`, no
vendored sources, and no `third_party/` directory.

## Principal finding

**`iramix_core` has zero third-party dependencies.** The engine links only
the C++20 standard library. Platform libraries (`avrt`, `ksuser`, `ole32`,
CoreAudio, JACK) reach only the `iramix_audio_probe` executable, never the
core library.

That is the strongest license and supply-chain position the project could
hold in the audio path, and it is worth stating as a property to defend
rather than an accident of an early build.

## Obligations recorded

| ID | Obligation | Why it matters |
|---|---|---|
| L-1 | Skiko runtime jars ship a 14 MB native DLL and ICU data with **no LICENSE or NOTICE file inside the jar** | The POM's Apache-2.0 covers Skiko's source, not the Skia (BSD-3-Clause) and ICU code statically linked into the binary. Both require attribution on redistribution, and the artifact supplies no text to copy. |
| L-2 | `libjack`'s license is unverified from this machine | The policy forbids GPL in a closed-source path. Upstream licenses the client library LGPL and the server GPL, which would make dynamic linking acceptable — but that is upstream reputation, not an inspected package, and R-13 leaves no Linux machine here to check on. |
| L-3 | CI actions pinned to mutable major tags (`@v7`, `@v5`, `@v6`) | Upstream can repoint these at any time, and the replacement would run with repository credentials while nothing here records the change. |
| L-4 | Two `org.jetbrains:annotations` versions coexist (13.0 compile, 23.0.0 runtime) | No license issue; a version skew worth resolving before it becomes a behavioural surprise. |

L-1 is the substantive one. It is not a paperwork detail: shipping the
Skiko runtime jar in a product redistributes Skia and ICU binaries under
licenses that require attribution the artifact itself does not carry.

## Entries resolved

- **Unit test framework** — no longer "to be selected". The project uses a
  hand-written harness and takes on no test-framework dependency.
- **Session serialization** — the deterministic binary schema is
  implemented at v4. SQLite remains a candidate for storage, not for the
  document format.

## Evidence boundary

This proves the declared-license position of every artifact the Java build
locks, that the C++ engine has no third-party dependencies, and that the
Java table can be regenerated and will fail on an unknown artifact.

It does not prove:

- **that any declared license is the complete license position.** A POM
  describes what the publisher declares for its own source. L-1 is a
  worked example of the gap: an Apache-2.0 artifact shipping BSD-3-Clause
  and Unicode-licensed binaries;
- the license of `libjack` as actually packaged (L-2), or of any Linux or
  macOS system component, none of which can be inspected from here (R-13);
- anything about vulnerabilities. The policy requires automated
  vulnerability inventory; **none exists**, and this work does not add one;
- that no transitive dependency is pulled in outside the Gradle lockfiles
  — the lockfiles are authoritative for the UI module only;
- license positions for candidate dependencies, which are recorded from
  upstream statements and must be re-verified when a candidate is actually
  adopted;
- that any of the four obligations has been discharged. All four remain
  open.
