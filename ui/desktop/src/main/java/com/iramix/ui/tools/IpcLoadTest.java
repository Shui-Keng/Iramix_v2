package com.iramix.ui.tools;

import com.iramix.ui.ipc.EngineSession;
import java.nio.file.Path;
import java.time.Duration;
import java.util.Arrays;
import java.util.Locale;

public final class IpcLoadTest {
    private static final int WARMUP_COMMANDS = 100;
    private static final int MEASURED_COMMANDS = 1_000;
    // The render loop is alive by construction, but nothing schedules it
    // against the ping loop. Wait for it to advance one more frame so the
    // measurement window opens on a loop proven to be producing frames.
    private static final Duration RENDER_READY_TIMEOUT =
        Duration.ofSeconds(10);
    // On a fast runner the window spans barely two frame periods
    // (macOS CI: 1,000 pings in ~73ms against a ~35ms frame), so a
    // healthy loop can legitimately complete no frame inside it. The
    // frame in flight during the window still proves the loop was not
    // wedged, so it is awaited and reported as distinct evidence.
    private static final Duration RENDER_DRAIN_TIMEOUT =
        Duration.ofSeconds(5);

    private IpcLoadTest() {}

    public static void main(String[] args) throws Exception {
        var engineProbe = System.getenv("IRAMIX_ENGINE_PROBE");
        if (engineProbe == null || engineProbe.isBlank()) {
            System.out.println(
                "IPC load test skipped; engine probe not supplied."
            );
            return;
        }

        try (
            var session = EngineSession.launch(Path.of(engineProbe));
            var uiLoad = DummyUiLoad.start()
        ) {
            for (var index = 0; index < WARMUP_COMMANDS; ++index) {
                session.ping();
            }

            var framesBeforeWait = uiLoad.renderedFrames();
            if (
                !uiLoad.awaitFrameAfter(
                    framesBeforeWait,
                    RENDER_READY_TIMEOUT
                )
            ) {
                throw new AssertionError(
                    "Dummy Skia UI render loop produced no frame in "
                        + RENDER_READY_TIMEOUT.toMillis()
                        + "ms before the measurement window opened; "
                        + "the render loop never reached steady state."
                );
            }

            var framesBeforeMeasurement = uiLoad.renderedFrames();
            var windowStart = System.nanoTime();
            var latencies = new long[MEASURED_COMMANDS];
            for (var index = 0; index < latencies.length; ++index) {
                latencies[index] = session.ping().toNanos();
            }
            var windowNanos = System.nanoTime() - windowStart;
            var measuredUiFrames =
                uiLoad.renderedFrames() - framesBeforeMeasurement;
            var frameEvidence = "IN_WINDOW";
            if (measuredUiFrames == 0) {
                if (
                    !uiLoad.awaitFrameAfter(
                        framesBeforeMeasurement,
                        RENDER_DRAIN_TIMEOUT
                    )
                ) {
                    throw new AssertionError(
                        "Dummy Skia UI render loop was producing frames "
                            + "before the measurement window but "
                            + "completed none during it or within "
                            + RENDER_DRAIN_TIMEOUT.toMillis()
                            + "ms after; the UI stalled under IPC load."
                    );
                }
                // Reported as zero on purpose: the frame completed
                // after the window closed and must not be counted as
                // though it had landed inside it.
                frameEvidence = "IN_FLIGHT_DRAINED";
            }

            Arrays.sort(latencies);
            System.out.printf(
                Locale.ROOT,
                "IPC load test: os=%s commands=%d warmup=%d "
                    + "uiFrames=%d uiFrameEvidence=%s window=%.3fms "
                    + "p50=%.3fms p95=%.3fms "
                    + "p99=%.3fms max=%.3fms%n",
                session.welcome().operatingSystem(),
                latencies.length,
                WARMUP_COMMANDS,
                measuredUiFrames,
                frameEvidence,
                milliseconds(windowNanos),
                milliseconds(percentile(latencies, 50)),
                milliseconds(percentile(latencies, 95)),
                milliseconds(percentile(latencies, 99)),
                milliseconds(latencies[latencies.length - 1])
            );
        }
    }

    private static long percentile(long[] sortedValues, int percentile) {
        var rank = (int) Math.ceil(
            percentile / 100.0 * sortedValues.length
        );
        return sortedValues[Math.max(0, rank - 1)];
    }

    private static double milliseconds(long nanoseconds) {
        return nanoseconds / 1_000_000.0;
    }
}
