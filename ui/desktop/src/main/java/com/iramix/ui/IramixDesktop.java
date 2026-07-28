package com.iramix.ui;

import java.awt.BorderLayout;
import java.awt.Dimension;
import java.util.concurrent.atomic.AtomicInteger;
import javax.swing.JFrame;
import javax.swing.SwingUtilities;
import javax.swing.WindowConstants;
import org.jetbrains.skia.PixelGeometry;
import org.jetbrains.skiko.SkiaLayer;
import org.jetbrains.skiko.SkiaLayerAnalytics;
import org.jetbrains.skiko.SkiaLayerProperties;
import org.jetbrains.skiko.SkiaLayerRenderDelegate;

public final class IramixDesktop {
    private IramixDesktop() {}

    public static void main(String[] args) {
        var frameLimit = parseFrameLimit(args);
        SwingUtilities.invokeLater(() -> showWindow(frameLimit));
    }

    private static int parseFrameLimit(String[] args) {
        for (var index = 0; index + 1 < args.length; ++index) {
            if ("--frames".equals(args[index])) {
                return Integer.parseInt(args[index + 1]);
            }
        }
        return 0;
    }

    private static void showWindow(int frameLimit) {
        var layer = new SkiaLayer(
            component -> component.getAccessibleContext(),
            new SkiaLayerProperties(),
            SkiaLayerAnalytics.Companion.getEmpty(),
            PixelGeometry.UNKNOWN
        );
        var window = new JFrame("Iramix — Phase 0");
        window.setDefaultCloseOperation(WindowConstants.EXIT_ON_CLOSE);
        window.setPreferredSize(new Dimension(1280, 720));
        window.getContentPane().setLayout(new BorderLayout());
        window.getContentPane().add(layer, BorderLayout.CENTER);
        window.pack();
        window.setLocationRelativeTo(null);
        window.setVisible(true);

        var remainingFrames = new AtomicInteger(frameLimit);
        var scene = new ReferenceScene(() -> {
            if (frameLimit <= 0) {
                return;
            }
            if (remainingFrames.decrementAndGet() <= 0) {
                SwingUtilities.invokeLater(() -> {
                    layer.dispose();
                    window.dispose();
                });
            } else {
                layer.needRender(true);
            }
        });
        layer.setRenderDelegate(new SkiaLayerRenderDelegate(layer, scene));
        layer.needRender(true);
    }
}
