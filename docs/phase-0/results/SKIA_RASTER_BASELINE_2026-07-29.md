# Skia Raster Baseline and Screenshot Comparison — 2026-07-29

Status: P0-005 second slice, and the first Week 3 evidence of any kind.
Until now the renderer had exactly one artefact — a reference window that
runs — and no result document. This one adds the three things Week 3 asks
for: a screenshot comparison, a documented text shaping and font fallback
behaviour, and a raster performance baseline.

## Scope

`RasterScene` draws a dense arrangement in logical coordinates: a
transport bar with five circular buttons and four stroked knob arcs, a
32-division ruler, eight lanes each carrying a filled waveform envelope
of 482 points and a six-segment cubic automation curve, and eight mixer
strips with faders and sixteen-segment meters. `RasterSpike` renders it
into an off-screen raster surface at 100%, 125%, 150%, and 200%, compares
each capture against a committed PNG baseline, and times 200 frames per
scale.

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\gradle.ps1 :ui:desktop:rasterSpike
```

The task is wired into `gradle check`, so it runs on every push on all
three CI operating systems. Baselines are regenerated with
`-PiramixRasterBaselines=write`.

## Three properties the scene had to have first

A screenshot baseline is worthless if the thing it captures can drift, so
the scene was constrained before it was measured:

- **No clock and no random source.** The waveform envelope and the
  automation curve are closed-form harmonic series of the lane index.
- **`StrictMath`, not `Math`.** `Math.sin` is only required to land
  within one ulp and may differ between JDK builds and CPU
  architectures. `StrictMath` is bit-exact by specification. A baseline
  built on `Math` would be a baseline for this machine's libm.
- **Arcs tessellated by hand.** Knob arcs are 48 straight segments rather
  than Skia arc calls, because an arc's flattening tolerance is an
  implementation detail and does not belong inside a baseline.

`repeatable=true` on every line below is the check that this held: the
scene is rendered twice into the same surface and the two readbacks must
be byte-identical before any comparison is trusted.

Pixels are also never compared in surface order. The surface is N32
premultiplied, which is BGRA on Windows and RGBA elsewhere; readback goes
through an explicit `RGBA_8888` image info so a capture from one
operating system is byte-comparable against another. Skipping that would
have produced channel-swapped differences that look exactly like a
rendering regression.

## Local result

Toolchain: Eclipse Temurin 21.0.11+10, Gradle 9.6.1, Skiko 0.150.1
(`skiko-awt-runtime-windows-x64`), HotSpot default GC. Hardware: AMD
Athlon Silver 3050U, 2 cores. Windows-only per R-13. All four scales are
warmed for 120 frames each *before* any is measured — see "An ordering
artefact" below.

```text
Raster spike: target=windows-x64 scale=100% size=1440x900
logical=1440x900 megapixels=1.296 repeatable=true baseline=match
diffPixels=0 maxDelta=0
digest=20b8d6cf5a82d56689b80aa19005a300e7023b1a21c7e1e2d701cdd7917b4cf8
frames=200 p50=6.007ms p95=7.607ms p99=10.872ms max=15.803ms
p50PerMpx=4.635ms gcCount=0 gcMillis=0

Raster spike: target=windows-x64 scale=125% size=1800x1125
logical=1440x900 megapixels=2.025 repeatable=true baseline=match
diffPixels=0 maxDelta=0
digest=aa50336badf5bebc798d1df686103d44ae77734880e46d808e93f943956d3ae6
frames=200 p50=7.580ms p95=8.671ms p99=11.355ms max=24.784ms
p50PerMpx=3.743ms gcCount=1 gcMillis=2

Raster spike: target=windows-x64 scale=150% size=2160x1350
logical=1440x900 megapixels=2.916 repeatable=true baseline=match
diffPixels=0 maxDelta=0
digest=8fd7c8e0fcc48c04c456b686689f831bf20d85d07164858a69263d57ea9b3ac2
frames=200 p50=9.035ms p95=12.284ms p99=17.757ms max=30.489ms
p50PerMpx=3.098ms gcCount=0 gcMillis=0

Raster spike: target=windows-x64 scale=200% size=2880x1800
logical=1440x900 megapixels=5.184 repeatable=true baseline=match
diffPixels=0 maxDelta=0
digest=ec2705425c2d948b7a307b70cfdd6b105e0afb69df900c311c3cd7b3e386480a
frames=200 p50=11.970ms p95=14.814ms p99=19.771ms max=23.531ms
p50PerMpx=2.309ms gcCount=1 gcMillis=3
```

The four digests were identical across five separate JVM invocations, so
determinism holds across processes and not merely within one.

## The comparison was verified by breaking it

A comparator that never fails proves nothing. `ACCENT` was changed from
`0xFF3BAE91` to `0xFF3BAE92` — one least-significant bit on one channel
of one colour, invisible on screen — and the run failed:

```text
scale=100% baseline=differs diffPixels=12753 maxDelta=1
scale=125% baseline=differs diffPixels=19829 maxDelta=1
scale=150% baseline=differs diffPixels=27866 maxDelta=1
scale=200% baseline=differs diffPixels=48652 maxDelta=1
Exception in thread "main" java.lang.AssertionError: 4 raster scale(s)
did not match their baseline.
```

The change was reverted and the digests returned to the values above.
Baselines are stored as `ImageIO` PNG rather than through Skia's encoder:
the point of the baseline is to detect a change in Skia's output, so the
storage format must not itself be a Skia artefact.

## Text shaping and font fallback

Text is deliberately **excluded from the baselines**. Glyph outlines and
hinting come from fonts installed on the host, so a pixel baseline for
text would encode this workstation's font set and fail everywhere else
for reasons that are not regressions. What is portable is the behaviour:

```text
Raster text: target=windows-x64 family=Segoe UI typefaceGlyphs=5394
familiesAvailable=230 samples=5

Raster text sample: label=ascii script=Latn codepoints=22
shapedGlyphs=22 missingGlyphs=0 width=133.01 fallback=Segoe UI
Raster text sample: label=latin-diacritics script=Latn codepoints=21
shapedGlyphs=21 missingGlyphs=0 width=144.83 fallback=Segoe UI
Raster text sample: label=cjk script=Jpan codepoints=5 shapedGlyphs=5
missingGlyphs=5 width=57.39 fallback=Yu Gothic UI
Raster text sample: label=rtl-arabic script=Arab codepoints=10
shapedGlyphs=10 missingGlyphs=0 width=75.15 fallback=Segoe UI
Raster text sample: label=emoji-astral script=Zsye codepoints=2
shapedGlyphs=2 missingGlyphs=2 width=38.45 fallback=Segoe UI Emoji
```

**`TextLine.make` does not fall back.** This is the finding. Segoe UI
resolves as the UI face and covers Latin, diacritics, and Arabic — but
for Japanese and for emoji, `missingGlyphs` equals the full codepoint
count: every glyph is `.notdef`. The line still reports a plausible
non-zero width, so a caller that only measured would never notice it was
about to draw five empty boxes.

The font manager *can* resolve both — it names `Yu Gothic UI` for
U+97F3 and `Segoe UI Emoji` for U+1F39B — but nothing connects the two.
Skia's single-font shaping path and the manager's per-codepoint matching
are separate mechanisms, and **Iramix must drive the fallback itself**.
That is a Phase 1 requirement, not a defect in Skiko.

`shapedGlyphs=10` for the Arabic sample against 10 codepoints also means
no ligature or contextual form was applied on that path; Arabic needs
joining behaviour that a per-codepoint mapping does not provide. The
sample is recorded as a marker, not as a claim that RTL works.

## Cost, against a frame budget

A 60 Hz budget is 16.67 ms and 120 Hz is 8.33 ms. This is a **full-scene
repaint with no damage regions, no layer caching, and no GPU**:

| Scale | Pixels | p50 | % of 60 Hz | % of 120 Hz |
|---|---|---:|---:|---:|
| 100% | 1.30 Mpx | 6.01 ms | 36% | 72% |
| 125% | 2.03 Mpx | 7.58 ms | 45% | 91% |
| 150% | 2.92 Mpx | 9.04 ms | 54% | 108% |
| 200% | 5.18 Mpx | 11.97 ms | 72% | 144% |

**CPU raster full-repaint misses 120 Hz at 150% scale and above on this
hardware, and has no headroom at 60 Hz once anything else is added.**
That is the concrete input to the Week 4 GPU spike, and it is measured
rather than assumed.

`p50PerMpx` falling from 4.64 ms to 2.31 ms as resolution rises is the
expected shape: the scene's geometry work — building 8 waveform paths of
482 points, 8 cubic curves, and 40 arcs — is counted in logical space and
so is constant across scales, while only rasterization grows with pixel
count. At 100% the fixed cost dominates.

## An ordering artefact, and a hypothesis that was wrong

Two measurement mistakes are recorded here because both would have
produced publishable-looking numbers.

**Warming each scale immediately before measuring it** made the first
scale absorb the JIT cost of the shared drawing code. The resulting curve
fell as resolution rose — 150% reported p95 28.4 ms against 200%'s
17.3 ms — which is physically backwards. Warming all four scales before
measuring any of them removed it, and p50 is now monotonic in pixel
count.

**Garbage collection was the obvious suspect for the frame-time tail**
and it is not the cause. `gcCount`/`gcMillis` were added around the
measured window specifically to check, and they refute it: the 100% and
150% windows report **zero collections** while showing worst frames of
15.8 ms and 30.5 ms, and the largest total GC time in any window is 3 ms
against tails that have reached 85 ms. The tail is unattributed. The most
plausible remaining explanation is host scheduling on an unquiesced
two-core developer laptop, but that is a guess and is labelled as one.

Consequently **p50 is the only figure here that reproduces**: across five
runs it varied by under 0.5 ms at every scale, while `max` varied between
15.8 ms and 85.3 ms at the same scale.

## Evidence boundary

This proves that a dense arrangement scene renders deterministically to
Skia's CPU raster backend on Windows, that a one-bit change is caught by
the committed baselines at all four scales, and that full-scene CPU
repaint costs 6–12 ms p50 on this hardware.

It does not prove:

- **anything about the GPU.** This is `Surface.makeRasterN32Premul` —
  software rasterization into a heap surface. No Direct3D, Metal, Vulkan,
  or OpenGL backend is touched, no `DirectContext` is created, and R-03
  remains open. Week 4 is unstarted;
- **anything about a window.** Nothing is presented. There is no swap
  chain, no vsync, no compositor, no resize, no monitor move, no
  sleep/wake, and no device-loss path. The number above is scene cost,
  not frame cost;
- **that HiDPI works.** Rendering at a 1.25 canvas scale is not the same
  as running on a 125% display. No AWT/Skiko surface, no
  `GraphicsConfiguration` transform, and no multi-monitor coordinate
  conversion is exercised — that part of Week 2 is still open;
- **that raster output is identical across operating systems.** The
  question is deliberately left open rather than assumed. Baselines are
  stored per Skiko target, and a target without one reports
  `baseline=absent` instead of failing. macOS and Linux CI legs will
  print their digests; comparing them against the four above is what
  would answer this, and it has not been done;
- **anything about text rendering.** Text is measured, never captured.
  No glyph is compared against a baseline at any scale, subpixel
  positioning and hinting are set but unverified, and the Arabic sample
  demonstrates only that shaping ran — not that it is correct;
- **anything about accessibility, IME, or input** (R-07). None is
  touched here;
- **anything about macOS or Linux performance** (R-13);
- **that the scene resembles the product.** It is a synthetic worst-case
  shaped like an arrangement, not the interaction prototypes Week 1 calls
  for, and the widget set it exercises is not a committed design.
