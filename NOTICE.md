# Third-Party Notices

Iramix redistributes third-party binary components. This file exists to
carry their required attribution.

**It is not yet complete.** See "Completeness boundary" at the end before
relying on it for a distributable build. Obligation L-1 in
[`docs/phase-0/DEPENDENCIES.md`](docs/phase-0/DEPENDENCIES.md) stays open
until that boundary closes.

## Skiko

`org.jetbrains.skiko:skiko-awt` and `skiko-awt-runtime-<target>`, version
0.150.1.

Licensed under the Apache License, Version 2.0, per the project's own
`LICENSE` file. Copyright JetBrains s.r.o. and contributors.

The Apache-2.0 grant covers Skiko's own source. It does **not** describe
the third-party code compiled into the native library the runtime jars
ship, which is what the rest of this file is about.

## Components inside the Skiko native library

Determined by inspecting the artifact this project actually pins —
`skiko-awt-runtime-windows-x64-0.150.1.jar`, whose `skiko-windows-x64.dll`
is 14,109,696 bytes and whose `icudtl.dat` is 10,468,208 bytes.

Evidence column meanings:

- **Copyright string** — an attribution notice is compiled into the
  shipped binary and was read from it verbatim. Strong.
- **Version string** — the component's own version marker appears in the
  binary. Strong for presence, silent about which licence version.
- **Name only** — the component's name appears in the binary. Consistent
  with linkage, but a symbol or an error message would look the same.

| Component | Licence | Evidence |
|---|---|---|
| Skia | BSD-3-Clause | Skiko is Skia's binding and the library is 14 MB of it; upstream `LICENSE` is BSD-3-Clause, "Copyright (c) 2011 Google Inc. All rights reserved." |
| ICU | Unicode licence | Copyright string, read from the `icudtl.dat` header |
| libpng | PNG Reference Library License v2 | Copyright string ×4 |
| libjpeg-turbo | IJG / BSD-3-Clause / Zlib | Copyright string |
| zlib | Zlib | Copyright string, plus `inflate 1.3.0.1` |
| Expat | MIT | Version string `expat_2.7.4` |
| HarfBuzz | MIT (Old MIT) | Name only |
| libwebp | BSD-3-Clause | Name only |
| Wuffs | Apache-2.0 | Name only |
| ANGLE | BSD-3-Clause | Name only |

### Attribution strings read from the shipped binaries

ICU, from the `icudtl.dat` header:

```text
Copyright (C) 2016 and later: Unicode, Inc. and others.
License & terms of use: http://www.unicode.org/copyright.html
```

libpng:

```text
Copyright (c) 2018-2026 Cosmin Truta
Copyright (c) 1998-2002,2004,2006-2018 Glenn Randers-Pehrson
Copyright (c) 1996-1997 Andreas Dilger
Copyright (c) 1995-1996 Guy Eric Schalnat, Group 42, Inc.
```

libjpeg-turbo:

```text
Copyright (C) 1991-2024 The libjpeg-turbo Project and many others
```

zlib:

```text
Copyright 1995-2023 Jean-loup Gailly and Mark Adler
Copyright 1995-2023 Mark Adler
```

## Steinberg VST3 and ASIO SDKs

Both are proprietary and supplied outside this repository; neither is
redistributed by this project. Their terms are recorded in
`docs/phase-0/DEPENDENCIES.md`. The ASIO developer agreement is not yet
signed (R-09) and must be before ASIO enters a public build.

## Completeness boundary

This file is assembled from **artifact inspection, not an upstream
manifest**, and the difference matters:

1. **Only one of five targets was inspected.** The Gradle cache holds the
   native jar for `windows-x64` only; the other four resolve to POMs
   until a build selects them. Linux and macOS builds may link components
   Windows does not — FreeType is the obvious candidate, and it is absent
   from the Windows library. Every shipped target needs its own scan.
2. **String scanning cannot prove absence.** A component linked without
   leaving a copyright or version string in the binary would not appear
   in the table above. Four entries rest on a name alone.
3. **Licence *versions* are inferred.** The table names each project's
   licence as upstream publishes it today. The pinned Skiko 0.150.1 was
   built at a fixed point against fixed component versions, and the exact
   licence text in force for those versions was not retrieved
   per-component.
4. **No upstream third-party notice set was obtained.** That remains the
   action that actually discharges L-1: Skiko's build manifest states
   what it links, and no amount of scanning substitutes for it.

Do not ship a distributable build against this file alone.
