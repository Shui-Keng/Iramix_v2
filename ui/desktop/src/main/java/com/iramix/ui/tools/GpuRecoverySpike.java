package com.iramix.ui.tools;

import com.iramix.ui.gpu.GpuRecoveryFrame;
import java.awt.Dimension;
import java.awt.GraphicsEnvironment;
import java.util.List;
import java.util.Locale;

/**
 * Phase 0 GPU recovery spike, slice 2: continuously render while
 * resizing and, when real devices exist, moving between monitors.
 */
public final class GpuRecoverySpike {
    private static final int PRESENTS_PER_STAGE = 3;
    private static final List<Dimension> RESIZE_SEQUENCE = List.of(
        new Dimension(960, 600),
        new Dimension(640, 400),
        new Dimension(1200, 750),
        new Dimension(320, 200),
        new Dimension(1024, 640),
        new Dimension(800, 500),
        new Dimension(1152, 720),
        new Dimension(480, 300),
        new Dimension(640, 400)
    );

    private GpuRecoverySpike() {}

    public static void main(String[] args) throws Exception {
        var target = System.getenv("IRAMIX_SKIKO_TARGET");
        if (target == null || target.isBlank()) {
            target = "unknown";
        }

        if (GraphicsEnvironment.isHeadless()) {
            System.out.println(
                "GPU recovery limitation: no display is available "
                    + "(java.awt.headless=true); resize and "
                    + "monitor-move were not exercised."
            );
            return;
        }

        try (var recovery = GpuRecoveryFrame.open()) {
            var resize = recovery.exerciseResizes(
                RESIZE_SEQUENCE,
                PRESENTS_PER_STAGE
            );
            var monitorMove = recovery.exerciseMonitorMoves(
                PRESENTS_PER_STAGE
            );
            System.out.printf(
                Locale.ROOT,
                "GPU recovery: target=%s backend=%s resizeStages=%d "
                    + "resizeTransitions=%d renderCallbacks=%d "
                    + "completedPresents=%d sizes=[%s] "
                    + "monitorCount=%d monitorVisited=%d "
                    + "monitorPresents=%d monitorMove=%s%n",
                target,
                recovery.graphicsApi(),
                resize.stages(),
                resize.transitions(),
                resize.renderCallbacks(),
                resize.completedPresents(),
                resize.observations(),
                monitorMove.monitorCount(),
                monitorMove.visitedMonitors(),
                monitorMove.completedPresents(),
                monitorMove.status()
            );
        }
    }
}
