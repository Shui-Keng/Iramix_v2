package com.iramix.ui.tools;

import com.iramix.ui.gpu.GpuFrame;
import com.iramix.ui.raster.RasterScene;
import java.awt.GraphicsEnvironment;
import java.lang.management.ManagementFactory;
import java.util.Arrays;
import java.util.Locale;

/**
 * The Phase 0 GPU rendering spike (Week 4, R-03), slice 1.
 *
 * <p>{@code RasterSpike} measures Skia's CPU raster backend into an
 * off-screen surface. This measures the same {@code RasterScene}
 * through a real, visible {@code SkiaLayer} window — Skiko's
 * hardware-accelerated path, backed by whichever {@code GraphicsApi}
 * Skiko actually selects on this host (Direct3D/ANGLE/OpenGL on
 * Windows, Metal on macOS, OpenGL on Linux, or a documented software
 * fallback if none of those are available). Frame time is the gap
 * between consecutive {@code onRender} calls as Skiko's own redraw
 * scheduler drives them, not the cost of one synchronous call.
 *
 * <p>No pixel baseline is compared here. GPU rasterization,
 * anti-aliasing, and blending are permitted to differ from the CPU
 * path and across GPU vendors, so a byte-identity check would not be
 * measuring anything stable — see {@code RasterSpike} for the
 * screenshot comparison. What is recorded is the backend selected and
 * the frame-time distribution.
 *
 * <p>This requires a real display. On a host with none —
 * {@code java.awt.headless=true} and no virtual display such as Xvfb
 * — the spike reports that it was skipped rather than failing, the
 * same pattern {@code IpcLoadTest} uses when no engine probe is
 * supplied.
 */
public final class GpuSpike {
    private static final int WARMUP_FRAMES = 60;
    private static final int MEASURED_FRAMES = 200;

    private GpuSpike() {}

    public static void main(String[] args) throws Exception {
        var target = System.getenv("IRAMIX_SKIKO_TARGET");
        if (target == null || target.isBlank()) {
            target = "unknown";
        }

        if (GraphicsEnvironment.isHeadless()) {
            System.out.println(
                "GPU spike skipped; no display is available "
                    + "(java.awt.headless=true)."
            );
            return;
        }

        var collectionsBefore = garbageCollections();
        var collectionMillisBefore = garbageCollectionMillis();
        try (
            var gpuFrame =
                GpuFrame.render(WARMUP_FRAMES + MEASURED_FRAMES)
        ) {
            var collections = garbageCollections() - collectionsBefore;
            var collectionMillis =
                garbageCollectionMillis() - collectionMillisBefore;

            var timestamps = gpuFrame.timestampsNanos();
            var periods = new long[MEASURED_FRAMES];
            for (var index = 0; index < periods.length; ++index) {
                periods[index] = timestamps[WARMUP_FRAMES + index]
                    - timestamps[WARMUP_FRAMES + index - 1];
            }
            Arrays.sort(periods);

            System.out.printf(
                Locale.ROOT,
                "GPU spike: target=%s backend=%s size=%dx%d "
                    + "logical=%dx%d frames=%d p50=%.3fms p95=%.3fms "
                    + "p99=%.3fms max=%.3fms gcCount=%d gcMillis=%d%n",
                target,
                gpuFrame.graphicsApi(),
                gpuFrame.widthPixels(),
                gpuFrame.heightPixels(),
                RasterScene.LOGICAL_WIDTH,
                RasterScene.LOGICAL_HEIGHT,
                periods.length,
                milliseconds(percentile(periods, 50)),
                milliseconds(percentile(periods, 95)),
                milliseconds(percentile(periods, 99)),
                milliseconds(periods[periods.length - 1]),
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

    private static long percentile(long[] sorted, int percentile) {
        var rank = (int) Math.ceil(percentile / 100.0 * sorted.length);
        return sorted[Math.max(0, rank - 1)];
    }

    private static double milliseconds(long nanoseconds) {
        return nanoseconds / 1_000_000.0;
    }
}
