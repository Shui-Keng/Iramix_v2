package com.iramix.ui;

import org.jetbrains.skia.Canvas;
import org.jetbrains.skia.Paint;
import org.jetbrains.skia.Rect;
import org.jetbrains.skiko.SkikoRenderDelegate;

public final class ReferenceScene implements SkikoRenderDelegate {
    private static final int BACKGROUND = (int) 0xFF0E1013L;
    private static final int ACCENT = (int) 0xFF3BAE91L;
    private static final int DIVIDER = (int) 0xFF343B47L;
    private static final int CLIP = (int) 0xFF4F61BFL;

    private final Paint paint = new Paint();
    private final Runnable afterFrame;

    public ReferenceScene(Runnable afterFrame) {
        this.afterFrame = afterFrame;
    }

    @Override
    public void onRender(
        Canvas canvas,
        int width,
        int height,
        long nanoTime
    ) {
        canvas.clear(BACKGROUND);
        fill(canvas, Rect.makeXYWH(24.0f, 24.0f, 320.0f, 72.0f), ACCENT);
        fill(
            canvas,
            Rect.makeXYWH(24.0f, 112.0f, width - 48.0f, 2.0f),
            DIVIDER
        );
        fill(
            canvas,
            Rect.makeXYWH(24.0f, 136.0f, width * 0.58f, 54.0f),
            CLIP
        );
        afterFrame.run();
    }

    private void fill(Canvas canvas, Rect rectangle, int color) {
        paint.setColor(color);
        canvas.drawRect(rectangle, paint);
    }
}
