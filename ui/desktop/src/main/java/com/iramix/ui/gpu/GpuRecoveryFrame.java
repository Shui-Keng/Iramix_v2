package com.iramix.ui.gpu;

import com.iramix.ui.raster.RasterScene;
import java.awt.BorderLayout;
import java.awt.Dimension;
import java.awt.GraphicsDevice;
import java.awt.GraphicsEnvironment;
import java.awt.Rectangle;
import java.lang.reflect.InvocationTargetException;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;
import javax.swing.JFrame;
import javax.swing.SwingUtilities;
import javax.swing.WindowConstants;
import org.jetbrains.skia.Canvas;
import org.jetbrains.skia.PixelGeometry;
import org.jetbrains.skiko.GraphicsApi;
import org.jetbrains.skiko.OS;
import org.jetbrains.skiko.SkiaLayer;
import org.jetbrains.skiko.SkiaLayerAnalytics;
import org.jetbrains.skiko.SkiaLayerProperties;
import org.jetbrains.skiko.SkikoRenderDelegate;

/**
 * Keeps a real {@link SkiaLayer} rendering while its owning window
 * changes size and graphics device.
 *
 * <p>The render delegate alone is not enough evidence for recovery:
 * Skiko records that callback into a picture before the backend draws
 * it. The analytics probe therefore marks a stage complete only after
 * {@code afterFrameRender}, which Skiko invokes after the backend
 * context has drawn and presented the frame.
 */
public final class GpuRecoveryFrame implements AutoCloseable {
    private static final long STAGE_TIMEOUT_NANOS =
        10_000_000_000L;
    private static final long PROGRESS_WAIT_MILLIS = 100L;

    private final Object progress = new Object();
    private final AtomicLong renderCallbacks = new AtomicLong();
    private final AtomicLong completedPresents = new AtomicLong();
    private final AtomicLong nextContextId = new AtomicLong();
    private final AtomicLong contextInitializations = new AtomicLong();
    private final AtomicReference<Throwable> failure =
        new AtomicReference<>();
    private final ThreadLocal<BackendFrame> drawingFrame =
        new ThreadLocal<>();
    private final JFrame frame;
    private final SkiaLayer layer;

    private volatile boolean running = true;
    private volatile long lastInitializedContext;
    private long activeGeneration;
    private long matchedGeneration;
    private long expectedContextId;
    private int expectedWidthPixels;
    private int expectedHeightPixels;
    private int stageCallbacks;
    private int stagePresents;

    private GpuRecoveryFrame() throws Exception {
        var builtFrame = new JFrame[1];
        var builtLayer = new SkiaLayer[1];
        SwingUtilities.invokeAndWait(() -> {
            var newLayer = new SkiaLayer(
                component -> component.getAccessibleContext(),
                new SkiaLayerProperties(),
                new RecoveryAnalytics(),
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

            var newFrame = new JFrame("Iramix GPU recovery spike");
            newFrame.setUndecorated(true);
            newFrame.setDefaultCloseOperation(
                WindowConstants.DISPOSE_ON_CLOSE
            );
            newFrame.setPreferredSize(new Dimension(960, 600));
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

    /** Opens a continuously rendering recovery window. */
    public static GpuRecoveryFrame open() throws Exception {
        return new GpuRecoveryFrame();
    }

    /**
     * Resizes the window through every supplied logical size and waits
     * for real backend presents at each resulting pixel size.
     */
    public ResizeResult exerciseResizes(
        List<Dimension> logicalSizes,
        int presentsPerStage
    ) throws Exception {
        if (logicalSizes.isEmpty()) {
            throw new IllegalArgumentException(
                "At least one resize stage is required."
            );
        }
        if (presentsPerStage < 1) {
            throw new IllegalArgumentException(
                "At least one present per stage is required."
            );
        }

        var observations = new ArrayList<String>();
        var callbacksBefore = renderCallbacks.get();
        var presentsBefore = completedPresents.get();
        for (var logicalSize : logicalSizes) {
            var target = resizeOnEdt(logicalSize);
            beginStage(
                target.widthPixels(),
                target.heightPixels(),
                lastInitializedContext
            );
            requestRender();
            awaitStage(presentsPerStage, "resize " + logicalSize.width
                + "x" + logicalSize.height);
            observations.add(String.format(
                Locale.ROOT,
                "%dx%d->%dx%d@%.2f",
                logicalSize.width,
                logicalSize.height,
                target.widthPixels(),
                target.heightPixels(),
                target.contentScale()
            ));
        }

        return new ResizeResult(
            logicalSizes.size(),
            logicalSizes.size() - 1,
            renderCallbacks.get() - callbacksBefore,
            completedPresents.get() - presentsBefore,
            String.join(";", observations)
        );
    }

    /**
     * Moves the window to every real AWT graphics device. A single
     * device is reported as a limitation, not a successful test.
     */
    public MonitorMoveResult exerciseMonitorMoves(
        int presentsPerMonitor
    ) throws Exception {
        var devices = GraphicsEnvironment
            .getLocalGraphicsEnvironment()
            .getScreenDevices();
        if (devices.length < 2) {
            return new MonitorMoveResult(
                devices.length,
                0,
                0L,
                MonitorMoveStatus.LIMITATION_SINGLE_MONITOR
            );
        }

        var presentsBefore = completedPresents.get();
        var visited = 0;
        for (var device : devices) {
            moveToDevice(device);
            var target = currentTargetOnEdt();
            beginStage(
                target.widthPixels(),
                target.heightPixels(),
                lastInitializedContext
            );
            requestRender();
            awaitStage(
                presentsPerMonitor,
                "monitor " + device.getIDstring()
            );
            var observedDevice = graphicsDeviceOnEdt();
            if (!device.getIDstring().equals(
                observedDevice.getIDstring()
            )) {
                throw new IllegalStateException(
                    "Window did not enter requested monitor "
                        + device.getIDstring() + "; observed "
                        + observedDevice.getIDstring() + "."
                );
            }
            ++visited;
        }

        return new MonitorMoveResult(
            devices.length,
            visited,
            completedPresents.get() - presentsBefore,
            MonitorMoveStatus.VERIFIED_MULTI_MONITOR
        );
    }

    /**
     * Disposes and reinitializes the same SkiaLayer backend context,
     * then requires completed presents from every replacement context.
     *
     * <p>This is a deterministic context-loss proxy. It does not suspend
     * the operating system and is not evidence of literal sleep/wake.
     */
    public ContextRecreationResult exerciseContextRecreations(
        int cycles,
        int presentsPerCycle
    ) throws Exception {
        if (cycles < 1) {
            throw new IllegalArgumentException(
                "At least one context recreation is required."
            );
        }
        if (presentsPerCycle < 1) {
            throw new IllegalArgumentException(
                "At least one present per cycle is required."
            );
        }

        var callbacksBefore = renderCallbacks.get();
        var presentsBefore = completedPresents.get();
        var contextInitializationsBefore =
            contextInitializations.get();
        var backends = new ArrayList<String>();
        backends.add(graphicsApi().toString());

        for (var cycle = 1; cycle <= cycles; ++cycle) {
            var target = recreateContextOnEdt();
            beginStage(
                target.surface().widthPixels(),
                target.surface().heightPixels(),
                target.contextId()
            );
            requestRender();
            awaitStage(
                presentsPerCycle,
                "context recreation " + cycle
            );
            backends.add(graphicsApi().toString());
        }

        return new ContextRecreationResult(
            cycles,
            contextInitializations.get()
                - contextInitializationsBefore,
            renderCallbacks.get() - callbacksBefore,
            completedPresents.get() - presentsBefore,
            String.join(">", backends),
            SleepWakeEvidence.CONTEXT_RECREATE_PROXY,
            LiteralSleepWakeStatus.ACCEPTED_EVIDENCE_GAP
        );
    }

    /** The backend Skiko kept alive across the recovery sequence. */
    public GraphicsApi graphicsApi() {
        return layer.getRenderApi();
    }

    private SurfaceTarget resizeOnEdt(Dimension logicalSize)
        throws Exception {
        var target = new SurfaceTarget[1];
        SwingUtilities.invokeAndWait(() -> {
            frame.setSize(logicalSize);
            frame.validate();
            target[0] = currentTarget();
        });
        return target[0];
    }

    private SurfaceTarget currentTargetOnEdt() throws Exception {
        var target = new SurfaceTarget[1];
        SwingUtilities.invokeAndWait(
            () -> target[0] = currentTarget()
        );
        return target[0];
    }

    private SurfaceTarget currentTarget() {
        var contentScale = layer.getContentScale();
        return new SurfaceTarget(
            (int) (layer.getWidth() * contentScale),
            (int) (layer.getHeight() * contentScale),
            contentScale
        );
    }

    private void moveToDevice(GraphicsDevice device)
        throws Exception {
        var bounds = device
            .getDefaultConfiguration()
            .getBounds();
        SwingUtilities.invokeAndWait(() -> {
            var location = centeredLocation(bounds, frame.getSize());
            frame.setLocation(location.x, location.y);
            frame.validate();
            layer.needRender(false);
        });

        var deadline = System.nanoTime() + STAGE_TIMEOUT_NANOS;
        while (!device.getIDstring().equals(
            graphicsDeviceOnEdt().getIDstring()
        )) {
            checkFailure();
            if (System.nanoTime() >= deadline) {
                throw new IllegalStateException(
                    "Window did not move to monitor "
                        + device.getIDstring() + " within 10s."
                );
            }
            synchronized (progress) {
                progress.wait(PROGRESS_WAIT_MILLIS);
            }
        }
    }

    private GraphicsDevice graphicsDeviceOnEdt() throws Exception {
        var device = new GraphicsDevice[1];
        SwingUtilities.invokeAndWait(() -> device[0] =
            frame.getGraphicsConfiguration().getDevice());
        return device[0];
    }

    private static java.awt.Point centeredLocation(
        Rectangle bounds,
        Dimension size
    ) {
        var x = bounds.x + Math.max(0, (bounds.width - size.width) / 2);
        var y = bounds.y
            + Math.max(0, (bounds.height - size.height) / 2);
        return new java.awt.Point(x, y);
    }

    private ContextTarget recreateContextOnEdt() throws Exception {
        var previousContext = lastInitializedContext;
        SwingUtilities.invokeAndWait(() -> {
            layer.dispose();
            frame.getContentPane().remove(layer);
            frame.getContentPane().add(layer, BorderLayout.CENTER);
            frame.validate();
            layer.needRender(false);
        });

        var contextId = awaitContextAfter(previousContext);
        return new ContextTarget(currentTargetOnEdt(), contextId);
    }

    private long awaitContextAfter(long previousContext)
        throws Exception {
        var deadline = System.nanoTime() + STAGE_TIMEOUT_NANOS;
        synchronized (progress) {
            while (lastInitializedContext <= previousContext) {
                checkFailure();
                if (System.nanoTime() >= deadline) {
                    throw new IllegalStateException(
                        "Skiko did not initialize a replacement "
                            + "backend context within 10s."
                    );
                }
                progress.wait(PROGRESS_WAIT_MILLIS);
            }
            return lastInitializedContext;
        }
    }

    private void beginStage(
        int widthPixels,
        int heightPixels,
        long contextId
    ) {
        if (widthPixels < 1 || heightPixels < 1) {
            throw new IllegalStateException(
                "Resize produced an empty SkiaLayer surface: "
                    + widthPixels + "x" + heightPixels + "."
            );
        }
        if (contextId < 1L) {
            throw new IllegalStateException(
                "No initialized Skiko backend context is available."
            );
        }
        synchronized (progress) {
            ++activeGeneration;
            matchedGeneration = 0L;
            expectedContextId = contextId;
            expectedWidthPixels = widthPixels;
            expectedHeightPixels = heightPixels;
            stageCallbacks = 0;
            stagePresents = 0;
        }
    }

    private void requestRender() throws Exception {
        SwingUtilities.invokeAndWait(() -> layer.needRender(false));
    }

    private void awaitStage(
        int requiredPresents,
        String description
    ) throws Exception {
        var deadline = System.nanoTime() + STAGE_TIMEOUT_NANOS;
        synchronized (progress) {
            while (stageCallbacks < requiredPresents
                || stagePresents < requiredPresents) {
                checkFailure();
                if (System.nanoTime() >= deadline) {
                    throw new IllegalStateException(
                        "GPU recovery did not complete " + description
                            + ": callbacks=" + stageCallbacks
                            + ", completedPresents=" + stagePresents
                            + ", expected="
                            + expectedWidthPixels + "x"
                            + expectedHeightPixels + ", context="
                            + expectedContextId + " within 10s."
                    );
                }
                progress.wait(PROGRESS_WAIT_MILLIS);
            }
        }
    }

    private void recordFrame(
        SkiaLayer renderLayer,
        Canvas canvas,
        int width,
        int height
    ) {
        try {
            renderCallbacks.incrementAndGet();
            synchronized (progress) {
                if (width == expectedWidthPixels
                    && height == expectedHeightPixels
                    && activeGeneration > 0L) {
                    matchedGeneration = activeGeneration;
                    ++stageCallbacks;
                }
                progress.notifyAll();
            }

            var scaleX =
                width / (float) RasterScene.LOGICAL_WIDTH;
            var scaleY =
                height / (float) RasterScene.LOGICAL_HEIGHT;
            var savedCount = canvas.save();
            canvas.scale(scaleX, scaleY);
            RasterScene.render(canvas, null);
            canvas.restoreToCount(savedCount);

            if (running) {
                SwingUtilities.invokeLater(() -> {
                    if (running) {
                        renderLayer.needRender(true);
                    }
                });
            }
        } catch (RuntimeException | Error exception) {
            fail(exception);
        }
    }

    private void contextInitialized(long contextId) {
        contextInitializations.incrementAndGet();
        lastInitializedContext = contextId;
        synchronized (progress) {
            progress.notifyAll();
        }
    }

    private void beforeBackendFrame(long contextId) {
        synchronized (progress) {
            var generation =
                matchedGeneration == activeGeneration
                    && contextId == expectedContextId
                        ? activeGeneration
                        : 0L;
            drawingFrame.set(
                new BackendFrame(generation, contextId)
            );
        }
    }

    private void afterBackendFrame() {
        completedPresents.incrementAndGet();
        var backendFrame = drawingFrame.get();
        drawingFrame.remove();
        synchronized (progress) {
            if (backendFrame != null
                && backendFrame.generation() > 0L
                && backendFrame.generation() == activeGeneration
                && backendFrame.contextId() == expectedContextId) {
                ++stagePresents;
            }
            progress.notifyAll();
        }
    }

    private void fail(Throwable exception) {
        failure.compareAndSet(null, exception);
        running = false;
        synchronized (progress) {
            progress.notifyAll();
        }
    }

    private void checkFailure() {
        var cause = failure.get();
        if (cause != null) {
            throw new IllegalStateException(
                "GPU recovery rendering failed.",
                cause
            );
        }
    }

    @Override
    public void close() {
        running = false;
        try {
            SwingUtilities.invokeAndWait(() -> {
                layer.dispose();
                frame.dispose();
            });
        } catch (InterruptedException exception) {
            Thread.currentThread().interrupt();
        } catch (InvocationTargetException exception) {
            throw new IllegalStateException(
                "Failed to dispose the GPU recovery window.",
                exception
            );
        }
    }

    private final class RecoveryAnalytics
        implements SkiaLayerAnalytics {
        @Override
        public RendererAnalytics renderer(
            String skikoVersion,
            OS os,
            GraphicsApi api
        ) {
            return new RendererAnalytics() {};
        }

        @Override
        public DeviceAnalytics device(
            String skikoVersion,
            OS os,
            GraphicsApi api,
            String deviceName
        ) {
            var contextId = nextContextId.incrementAndGet();
            return new DeviceAnalytics() {
                @Override
                public void contextInit() {
                    contextInitialized(contextId);
                }

                @Override
                public void beforeFrameRender() {
                    beforeBackendFrame(contextId);
                }

                @Override
                public void afterFrameRender() {
                    afterBackendFrame();
                }
            };
        }
    }

    private record SurfaceTarget(
        int widthPixels,
        int heightPixels,
        float contentScale
    ) {}

    private record ContextTarget(
        SurfaceTarget surface,
        long contextId
    ) {}

    private record BackendFrame(
        long generation,
        long contextId
    ) {}

    /** Counters from the resize recovery sequence. */
    public record ResizeResult(
        int stages,
        int transitions,
        long renderCallbacks,
        long completedPresents,
        String observations
    ) {}

    /** Whether a real cross-device monitor move was exercised. */
    public enum MonitorMoveStatus {
        VERIFIED_MULTI_MONITOR,
        LIMITATION_SINGLE_MONITOR
    }

    /** Counters from the monitor-move recovery sequence. */
    public record MonitorMoveResult(
        int monitorCount,
        int visitedMonitors,
        long completedPresents,
        MonitorMoveStatus status
    ) {}

    /** What the deterministic Slice 3 proxy actually exercises. */
    public enum SleepWakeEvidence {
        CONTEXT_RECREATE_PROXY
    }

    /** Status of literal operating-system sleep/wake evidence. */
    public enum LiteralSleepWakeStatus {
        ACCEPTED_EVIDENCE_GAP
    }

    /** Counters from the deterministic context-loss proxy. */
    public record ContextRecreationResult(
        int cycles,
        long contextInitializations,
        long renderCallbacks,
        long completedPresents,
        String backends,
        SleepWakeEvidence sleepWake,
        LiteralSleepWakeStatus literalSleepWake
    ) {}
}
