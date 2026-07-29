package com.iramix.ui.tools;

import java.time.Duration;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;
import org.jetbrains.skia.Paint;
import org.jetbrains.skia.Surface;

final class DummyUiLoad implements AutoCloseable {
    private static final int WIDTH = 1920;
    private static final int HEIGHT = 1080;
    private static final int TRACKS = 200;
    private static final int CLIPS_PER_TRACK = 10;
    private static final Duration FIRST_FRAME_TIMEOUT =
        Duration.ofSeconds(5);

    private final AtomicBoolean running = new AtomicBoolean(true);
    private final AtomicLong renderedFrames = new AtomicLong();
    private final AtomicReference<Throwable> renderFailure =
        new AtomicReference<>();
    // Woken after every completed frame and once the render loop exits,
    // so a bounded waiter can never miss the wake-up it is waiting for.
    private final Object frameSignal = new Object();
    private final Thread renderThread;

    private DummyUiLoad() throws InterruptedException {
        renderThread = Thread.ofPlatform()
            .daemon(true)
            .name("iramix-dummy-ui-load")
            .start(this::renderLoop);
        var rendered = false;
        try {
            rendered = awaitFrameAfter(0L, FIRST_FRAME_TIMEOUT);
        } catch (IllegalStateException exception) {
            close();
            throw exception;
        }
        if (!rendered) {
            close();
            throw new IllegalStateException(
                "Dummy UI load did not render its first frame in time."
            );
        }
    }

    static DummyUiLoad start() throws InterruptedException {
        return new DummyUiLoad();
    }

    long renderedFrames() {
        return renderedFrames.get();
    }

    /**
     * Waits until the frame counter has moved past {@code baseline},
     * which proves the render loop is not merely alive but still
     * producing frames. Returns false if the timeout expires first;
     * throws if the renderer failed, because a dead renderer is a
     * different defect from a slow one.
     */
    boolean awaitFrameAfter(long baseline, Duration timeout)
            throws InterruptedException {
        var deadline = System.nanoTime() + timeout.toNanos();
        synchronized (frameSignal) {
            while (renderedFrames.get() <= baseline) {
                failIfRendererFailed();
                var remaining = deadline - System.nanoTime();
                if (remaining <= 0L) {
                    return false;
                }
                // Round up so a sub-millisecond remainder still waits
                // rather than degenerating into wait(0) = wait forever.
                frameSignal.wait(
                    Math.max(1L, remaining / 1_000_000L)
                );
            }
        }
        failIfRendererFailed();
        return true;
    }

    private void failIfRendererFailed() {
        var failure = renderFailure.get();
        if (failure != null) {
            throw new IllegalStateException(
                "Dummy UI renderer failed.",
                failure
            );
        }
    }

    @Override
    public void close() {
        running.set(false);
        try {
            renderThread.join(5_000);
        } catch (InterruptedException exception) {
            Thread.currentThread().interrupt();
        }
        if (renderThread.isAlive()) {
            throw new IllegalStateException(
                "Dummy UI load did not stop in time."
            );
        }
    }

    private void renderLoop() {
        try (
            var surface = Surface.Companion.makeRasterN32Premul(
                WIDTH,
                HEIGHT
            );
            var paint = new Paint()
        ) {
            var canvas = surface.getCanvas();
            while (running.get()) {
                canvas.clear((int) 0xFF0E1013L);
                for (var track = 0; track < TRACKS; ++track) {
                    var top = 4.0f + track * 5.0f;
                    paint.setColor(
                        (track & 1) == 0
                            ? (int) 0xFF28313DL
                            : (int) 0xFF222934L
                    );
                    canvas.drawRect(
                        0.0f,
                        top,
                        WIDTH,
                        top + 4.0f,
                        paint
                    );
                    paint.setColor((int) 0xFF4F61BFL);
                    for (
                        var clip = 0;
                        clip < CLIPS_PER_TRACK;
                        ++clip
                    ) {
                        var left = 8.0f + clip * 190.0f;
                        canvas.drawRect(
                            left,
                            top,
                            left + 174.0f,
                            top + 3.0f,
                            paint
                        );
                    }
                }
                surface.flush();
                renderedFrames.incrementAndGet();
                signalFrame();
                Thread.yield();
            }
        } catch (RuntimeException | LinkageError exception) {
            renderFailure.set(exception);
        } finally {
            // Publish the failure (or the shutdown) to any waiter that
            // would otherwise block for its full timeout.
            signalFrame();
        }
    }

    private void signalFrame() {
        synchronized (frameSignal) {
            frameSignal.notifyAll();
        }
    }
}
