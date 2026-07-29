package com.iramix.ui.tools;

import com.iramix.ui.raster.FrameBaseline;
import com.iramix.ui.raster.RasterFrame;
import com.iramix.ui.raster.RasterScene;
import com.iramix.ui.raster.TextShapingReport;
import java.lang.management.ManagementFactory;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Locale;

/**
 * The Phase 0 Skia raster spike.
 *
 * <p>Produces three kinds of evidence in one run, all as counter lines
 * on stdout:
 *
 * <ol>
 *   <li>a within-process determinism check — the same scene rendered
 *       twice must produce identical pixels, otherwise no baseline
 *       comparison below it means anything;</li>
 *   <li>a screenshot comparison against the committed baseline for this
 *       Skiko target at 100%, 125%, 150%, and 200% scale;</li>
 *   <li>a raster frame-time baseline per scale.</li>
 * </ol>
 *
 * <p>Baselines are stored per Skiko target, mirroring the per-target
 * dependency lockfiles. Whether CPU raster output is bit-identical
 * across operating systems is an open question this project has not
 * measured, so the tool does not assume it: a target with no committed
 * baseline reports {@code baseline=absent} and does not fail. The
 * digest is printed on every host regardless, which is what makes the
 * cross-platform comparison answerable from CI logs later.
 *
 * <p>All four scales are warmed before any of them is measured. Warming
 * per scale immediately before measuring it made the first scale absorb
 * the JIT cost of the shared drawing code and produced a frame-time
 * curve that fell as resolution rose — an ordering artefact, not a
 * property of the renderer.
 */
public final class RasterSpike {
    private static final float[] SCALES = {
        1.00f,
        1.25f,
        1.50f,
        2.00f,
    };

    private static final int WARMUP_FRAMES = 120;
    private static final int MEASURED_FRAMES = 200;

    private RasterSpike() {}

    /** What one scale produced, before timings are taken. */
    private record Capture(
        RasterFrame frame,
        int percent,
        boolean repeatable,
        String verdict,
        long differingPixels,
        int maxChannelDelta,
        String digest
    ) {}

    public static void main(String[] args) {
        var target = System.getenv("IRAMIX_SKIKO_TARGET");
        if (target == null || target.isBlank()) {
            target = "unknown";
        }
        var baselineRoot = System.getenv("IRAMIX_RASTER_BASELINES");
        if (baselineRoot == null || baselineRoot.isBlank()) {
            System.out.println(
                "Raster spike skipped; baseline directory not supplied."
            );
            return;
        }
        var write = "write".equals(
            System.getenv("IRAMIX_RASTER_BASELINE_MODE")
        );
        var directory = Path.of(baselineRoot).resolve(target);

        var captures = new ArrayList<Capture>();
        try {
            for (var scale : SCALES) {
                captures.add(capture(directory, scale, write));
            }
            warmAll(captures);
            report(target, captures);
        } finally {
            for (var capture : captures) {
                capture.frame().close();
            }
        }

        reportText(target);
        verify(captures);
    }

    private static Capture capture(
        Path directory,
        float scale,
        boolean write
    ) {
        var percent = Math.round(scale * 100.0f);
        var frame = new RasterFrame(scale);
        frame.render(null);
        var first = frame.readRgb();

        // Render again into the same surface. If two consecutive
        // renders of a scene that reads no clock and no random source
        // differ, the scene is not a valid baseline origin and every
        // comparison below it would be noise.
        frame.render(null);
        var repeatable = Arrays.equals(first, frame.readRgb());

        var file = directory.resolve(
            "raster-scene-" + percent + ".png"
        );
        String verdict;
        var differingPixels = 0L;
        var maxDelta = 0;
        if (write) {
            FrameBaseline.write(
                file,
                frame.widthPixels(),
                frame.heightPixels(),
                first
            );
            verdict = "written";
        } else if (!Files.isRegularFile(file)) {
            verdict = "absent";
        } else {
            var difference = FrameBaseline.compare(
                FrameBaseline.read(file),
                frame.widthPixels(),
                frame.heightPixels(),
                first
            );
            differingPixels = difference.differingPixels();
            maxDelta = difference.maxChannelDelta();
            verdict = difference.identical() ? "match" : "differs";
        }

        return new Capture(
            frame,
            percent,
            repeatable,
            verdict,
            differingPixels,
            maxDelta,
            FrameBaseline.digest(first)
        );
    }

    private static void warmAll(List<Capture> captures) {
        for (var pass = 0; pass < WARMUP_FRAMES; ++pass) {
            for (var capture : captures) {
                capture.frame().render(null);
            }
        }
    }

    private static void report(
        String target,
        List<Capture> captures
    ) {
        for (var capture : captures) {
            var frame = capture.frame();
            var timings = new long[MEASURED_FRAMES];
            // Collections are counted around the measured window so the
            // frame-time tail can be attributed rather than guessed at.
            // The scene allocates Skia paths per lane per frame, which
            // made garbage collection the obvious suspect for the tail —
            // and these counters rule it out: windows with a worst frame
            // of tens of milliseconds report zero collections. Whatever
            // produces the tail, it is not the JVM heap, and no claim
            // about it should be made without a counter behind it.
            var collectionsBefore = garbageCollections();
            var collectionMillisBefore = garbageCollectionMillis();
            for (var index = 0; index < timings.length; ++index) {
                var start = System.nanoTime();
                frame.render(null);
                timings[index] = System.nanoTime() - start;
            }
            var collections =
                garbageCollections() - collectionsBefore;
            var collectionMillis =
                garbageCollectionMillis() - collectionMillisBefore;
            Arrays.sort(timings);
            var megapixels = frame.widthPixels()
                / 1_000.0
                * frame.heightPixels()
                / 1_000.0;
            System.out.printf(
                Locale.ROOT,
                "Raster spike: target=%s scale=%d%% size=%dx%d "
                    + "logical=%dx%d megapixels=%.3f repeatable=%b "
                    + "baseline=%s diffPixels=%d maxDelta=%d "
                    + "digest=%s frames=%d p50=%.3fms p95=%.3fms "
                    + "p99=%.3fms max=%.3fms p50PerMpx=%.3fms "
                    + "gcCount=%d gcMillis=%d%n",
                target,
                capture.percent(),
                frame.widthPixels(),
                frame.heightPixels(),
                RasterScene.LOGICAL_WIDTH,
                RasterScene.LOGICAL_HEIGHT,
                megapixels,
                capture.repeatable(),
                capture.verdict(),
                capture.differingPixels(),
                capture.maxChannelDelta(),
                capture.digest(),
                timings.length,
                milliseconds(percentile(timings, 50)),
                milliseconds(percentile(timings, 95)),
                milliseconds(percentile(timings, 99)),
                milliseconds(timings[timings.length - 1]),
                milliseconds(percentile(timings, 50)) / megapixels,
                collections,
                collectionMillis
            );
        }
    }

    private static long garbageCollections() {
        var total = 0L;
        for (var bean
            : ManagementFactory.getGarbageCollectorMXBeans()) {
            var count = bean.getCollectionCount();
            if (count > 0L) {
                total += count;
            }
        }
        return total;
    }

    private static long garbageCollectionMillis() {
        var total = 0L;
        for (var bean
            : ManagementFactory.getGarbageCollectorMXBeans()) {
            var millis = bean.getCollectionTime();
            if (millis > 0L) {
                total += millis;
            }
        }
        return total;
    }

    private static void verify(List<Capture> captures) {
        for (var capture : captures) {
            if (!capture.repeatable()) {
                throw new AssertionError(
                    "Scene rendering is not repeatable at scale "
                        + capture.percent() + "%."
                );
            }
        }
        var mismatches = captures.stream()
            .filter(capture -> "differs".equals(capture.verdict()))
            .count();
        if (mismatches > 0L) {
            throw new AssertionError(
                mismatches
                    + " raster scale(s) did not match their baseline."
            );
        }
    }

    private static void reportText(String target) {
        var report = TextShapingReport.measure();
        System.out.printf(
            Locale.ROOT,
            "Raster text: target=%s family=%s typefaceGlyphs=%d "
                + "familiesAvailable=%d samples=%d%n",
            target,
            report.resolvedFamily(),
            report.typefaceGlyphs(),
            report.familiesAvailable(),
            report.samples().size()
        );
        for (var sample : report.samples()) {
            System.out.printf(
                Locale.ROOT,
                "Raster text sample: label=%s script=%s codepoints=%d "
                    + "shapedGlyphs=%d missingGlyphs=%d width=%.2f "
                    + "fallback=%s%n",
                sample.label(),
                sample.script(),
                sample.codepoints(),
                sample.shapedGlyphs(),
                sample.missingGlyphs(),
                sample.width(),
                sample.fallbackFamily()
            );
        }
    }

    private static long percentile(long[] sorted, int percentile) {
        var rank = (int) Math.ceil(percentile / 100.0 * sorted.length);
        return sorted[Math.max(0, rank - 1)];
    }

    private static double milliseconds(long nanoseconds) {
        return nanoseconds / 1_000_000.0;
    }
}
