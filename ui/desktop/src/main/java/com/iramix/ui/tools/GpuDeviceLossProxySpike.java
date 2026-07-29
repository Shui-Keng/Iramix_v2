package com.iramix.ui.tools;

import com.iramix.ui.gpu.GpuRecoveryFrame;
import java.awt.GraphicsEnvironment;
import java.util.Locale;

/**
 * Phase 0 GPU recovery spike, slice 4: a safe Skiko
 * RenderException/fallback proxy, explicitly not native surface
 * allocation failure, adapter removal, driver reset, or TDR.
 */
public final class GpuDeviceLossProxySpike {
    private static final int RECOVERED_PRESENTS = 3;

    private GpuDeviceLossProxySpike() {}

    public static void main(String[] args) throws Exception {
        var target = System.getenv("IRAMIX_SKIKO_TARGET");
        if (target == null || target.isBlank()) {
            target = "unknown";
        }

        if (GraphicsEnvironment.isHeadless()) {
            System.out.println(
                "GPU device-loss proxy limitation: no display is "
                    + "available (java.awt.headless=true); "
                    + "deviceLoss=NOT_EXERCISED "
                    + "nativeSurfaceFailure=ACCEPTED_EVIDENCE_GAP "
                    + "literalTdr=ACCEPTED_EVIDENCE_GAP."
            );
            return;
        }

        try (var recovery =
                GpuRecoveryFrame.openForDeviceLossProxy()) {
            var result = recovery.exerciseDeviceLossProxy(
                RECOVERED_PRESENTS
            );
            System.out.printf(
                Locale.ROOT,
                "GPU device-loss proxy: target=%s "
                    + "initialBackend=%s recoveredBackend=%s "
                    + "injectedFailures=%d "
                    + "contextInitializations=%d "
                    + "renderCallbacks=%d completedPresents=%d "
                    + "deviceLoss=%s nativeSurfaceFailure=%s "
                    + "literalTdr=%s%n",
                target,
                result.initialBackend(),
                result.recoveredBackend(),
                result.injectedFailures(),
                result.contextInitializations(),
                result.renderCallbacks(),
                result.completedPresents(),
                result.deviceLoss(),
                result.nativeSurfaceFailure(),
                result.literalTdr()
            );
        }
    }
}
