package com.iramix.ui.tools;

import com.iramix.ui.gpu.GpuRecoveryFrame;
import java.awt.GraphicsEnvironment;
import java.util.Locale;

/**
 * Phase 0 GPU recovery spike, slice 3: a deterministic proxy for
 * context loss, explicitly not literal operating-system sleep/wake.
 */
public final class GpuContextRecoverySpike {
    private static final int RECREATION_CYCLES = 5;
    private static final int PRESENTS_PER_CYCLE = 3;

    private GpuContextRecoverySpike() {}

    public static void main(String[] args) throws Exception {
        var target = System.getenv("IRAMIX_SKIKO_TARGET");
        if (target == null || target.isBlank()) {
            target = "unknown";
        }

        if (GraphicsEnvironment.isHeadless()) {
            System.out.println(
                "GPU context recovery limitation: no display is "
                    + "available (java.awt.headless=true); "
                    + "context-recreate proxy was not exercised and "
                    + "literalSleepWake=ACCEPTED_EVIDENCE_GAP."
            );
            return;
        }

        try (var recovery = GpuRecoveryFrame.open()) {
            var result = recovery.exerciseContextRecreations(
                RECREATION_CYCLES,
                PRESENTS_PER_CYCLE
            );
            System.out.printf(
                Locale.ROOT,
                "GPU context recovery: target=%s backend=%s cycles=%d "
                    + "contextInitializations=%d renderCallbacks=%d "
                    + "completedPresents=%d backends=[%s] "
                    + "sleepWake=%s literalSleepWake=%s%n",
                target,
                recovery.graphicsApi(),
                result.cycles(),
                result.contextInitializations(),
                result.renderCallbacks(),
                result.completedPresents(),
                result.backends(),
                result.sleepWake(),
                result.literalSleepWake()
            );
        }
    }
}
