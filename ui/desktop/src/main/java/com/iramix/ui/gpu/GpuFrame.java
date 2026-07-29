package com.iramix.ui.gpu;

import com.iramix.ui.raster.RasterScene;
import java.awt.BorderLayout;
import java.awt.Dimension;
import java.lang.reflect.InvocationTargetException;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import javax.swing.JFrame;
import javax.swing.SwingUtilities;
import javax.swing.WindowConstants;
import org.jetbrains.skia.Canvas;
import org.jetbrains.skia.PixelGeometry;
import org.jetbrains.skiko.GraphicsApi;
import org.jetbrains.skiko.SkiaLayer;
import org.jetbrains.skiko.SkiaLayerAnalytics;
import org.jetbrains.skiko.SkiaLayerProperties;
import org.jetbrains.skiko.SkikoRenderDelegate;

/**
 * Drives {@link RasterScene} through a real, hardware-accelerated
 * {@code SkiaLayer} window rather than an off-screen raster surface —
 * the Week 4 counterpart to {@code RasterFrame}. A frame here is
 * scheduled through Skiko's own redraw path and a real GPU (or
 * whatever fallback Skiko selects) rather than invoked synchronously,
 * so frame time is measured as the gap between consecutive {@code
 * onRender} calls, not the cost of one call.
 *
 * <p>The window is undecorated and sized to {@link
 * RasterScene#LOGICAL_WIDTH}x{@link RasterScene#LOGICAL_HEIGHT} so
 * that no window-manager chrome affects the reported surface size, but
 * the scene is still scaled to whatever size Skiko actually reports in
 * {@code onRender} — this host's display scaling is not assumed.
 */
public final class GpuFrame implements AutoCloseable {
    private static final long FRAME_TIMEOUT_SECONDS = 30;

    private final JFrame frame;
    private final SkiaLayer layer;
    private final long[] timestampsNanos;
    private final AtomicInteger frameIndex = new AtomicInteger();
    private final CountDownLatch done = new CountDownLatch(1);
    private final AtomicReference<Throwable> failure =
        new AtomicReference<>();
    private volatile int widthPixels;
    private volatile int heightPixels;

    private GpuFrame(int frameCount) throws Exception {
        this.timestampsNanos = new long[frameCount];
        var builtFrame = new JFrame[1];
        var builtLayer = new SkiaLayer[1];
        SwingUtilities.invokeAndWait(() -> {
            var newLayer = new SkiaLayer(
                component -> component.getAccessibleContext(),
                new SkiaLayerProperties(),
                SkiaLayerAnalytics.Companion.getEmpty(),
                PixelGeometry.UNKNOWN
            );
            newLayer.setRenderDelegate(new SkikoRenderDelegate() {
                @Override
                public void onRender(
                    Canvas canvas,
                    int width,
                    int height,
                    long nanoTime
                ) {
                    recordFrame(newLayer, canvas, width, height);
                }
            });

            var newFrame = new JFrame("Iramix GPU spike");
            newFrame.setUndecorated(true);
            newFrame.setDefaultCloseOperation(
                WindowConstants.DISPOSE_ON_CLOSE
            );
            newFrame.setPreferredSize(new Dimension(
                RasterScene.LOGICAL_WIDTH,
                RasterScene.LOGICAL_HEIGHT
            ));
            newFrame.getContentPane().setLayout(new BorderLayout());
            newFrame.getContentPane()
                .add(newLayer, BorderLayout.CENTER);
            newFrame.pack();
            newFrame.setLocationRelativeTo(null);
            newFrame.setVisible(true);
            newLayer.needRender(true);

            builtFrame[0] = newFrame;
            builtLayer[0] = newLayer;
        });
        this.frame = builtFrame[0];
        this.layer = builtLayer[0];
    }

    /**
     * Renders {@code frameCount} frames of {@link RasterScene} through
     * a real window and returns once all of them have been captured.
     */
    public static GpuFrame render(int frameCount) throws Exception {
        var gpuFrame = new GpuFrame(frameCount);
        var completed = gpuFrame.done.await(
            FRAME_TIMEOUT_SECONDS,
            TimeUnit.SECONDS
        );
        if (!completed) {
            gpuFrame.close();
            throw new IllegalStateException(
                "GPU spike did not complete " + frameCount
                    + " frames within " + FRAME_TIMEOUT_SECONDS
                    + "s; the window may not have received a display."
            );
        }
        if (gpuFrame.failure.get() != null) {
            var cause = gpuFrame.failure.get();
            gpuFrame.close();
            throw new IllegalStateException(
                "GPU spike rendering failed.",
                cause
            );
        }
        return gpuFrame;
    }

    private void recordFrame(
        SkiaLayer renderLayer,
        Canvas canvas,
        int width,
        int height
    ) {
        try {
            var index = frameIndex.getAndIncrement();
            if (index >= timestampsNanos.length) {
                return;
            }
            timestampsNanos[index] = System.nanoTime();
            widthPixels = width;
            heightPixels = height;

            var scale = width / (float) RasterScene.LOGICAL_WIDTH;
            var savedCount = canvas.save();
            canvas.scale(scale, scale);
            RasterScene.render(canvas, null);
            canvas.restoreToCount(savedCount);

            if (index + 1 < timestampsNanos.length) {
                // Scheduling the next frame from a fresh EDT dispatch
                // rather than calling needRender() directly here avoids
                // any reentrant call into Skiko's redraw path while it
                // is still on the stack finishing this one.
                SwingUtilities.invokeLater(
                    () -> renderLayer.needRender(true)
                );
            } else {
                done.countDown();
            }
        } catch (RuntimeException | Error exception) {
            failure.set(exception);
            done.countDown();
        }
    }

    /** Per-frame capture timestamps, in {@link System#nanoTime()} units. */
    public long[] timestampsNanos() {
        return timestampsNanos;
    }

    /** The GPU (or software fallback) backend Skiko actually selected. */
    public GraphicsApi graphicsApi() {
        return layer.getRenderApi();
    }

    public int widthPixels() {
        return widthPixels;
    }

    public int heightPixels() {
        return heightPixels;
    }

    @Override
    public void close() {
        try {
            SwingUtilities.invokeAndWait(() -> {
                layer.dispose();
                frame.dispose();
            });
        } catch (InterruptedException exception) {
            Thread.currentThread().interrupt();
        } catch (InvocationTargetException exception) {
            throw new IllegalStateException(
                "Failed to dispose the GPU spike window.",
                exception
            );
        }
    }
}
