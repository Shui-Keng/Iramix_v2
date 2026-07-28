package com.iramix.ui.tools;

import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import org.jetbrains.skia.Paint;
import org.jetbrains.skia.Surface;

final class DummyUiLoad implements AutoCloseable {
    private static final int WIDTH = 1920;
    private static final int HEIGHT = 1080;
    private static final int TRACKS = 200;
    private static final int CLIPS_PER_TRACK = 10;

    private final AtomicBoolean running = new AtomicBoolean(true);
    private final AtomicLong renderedFrames = new AtomicLong();
    private final AtomicReference<Throwable> renderFailure =
        new AtomicReference<>();
    private final CountDownLatch firstFrame = new CountDownLatch(1);
    private final Thread renderThread;

    private DummyUiLoad() throws InterruptedException {
        renderThread = Thread.ofPlatform()
            .daemon(true)
            .name("iramix-dummy-ui-load")
            .start(this::renderLoop);
        if (!firstFrame.await(5, TimeUnit.SECONDS)) {
            close();
            throw new IllegalStateException(
                "Dummy UI load did not render its first frame in time."
            );
        }
        if (renderFailure.get() != null) {
            close();
            throw new IllegalStateException(
                "Dummy UI renderer failed.",
                renderFailure.get()
            );
        }
    }

    static DummyUiLoad start() throws InterruptedException {
        return new DummyUiLoad();
    }

    long renderedFrames() {
        return renderedFrames.get();
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
                firstFrame.countDown();
                Thread.yield();
            }
        } catch (RuntimeException | LinkageError exception) {
            renderFailure.set(exception);
            firstFrame.countDown();
        }
    }
}
