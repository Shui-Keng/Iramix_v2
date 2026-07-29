package com.iramix.ui.raster;

import org.jetbrains.skia.Canvas;
import org.jetbrains.skia.Font;
import org.jetbrains.skia.Paint;
import org.jetbrains.skia.PaintMode;
import org.jetbrains.skia.PaintStrokeCap;
import org.jetbrains.skia.Path;
import org.jetbrains.skia.PathBuilder;
import org.jetbrains.skia.RRect;
import org.jetbrains.skia.TextLine;

/**
 * The Phase 0 raster reference scene.
 *
 * <p>The scene exists to be captured, not to be pretty: every drawing
 * command must produce the same pixels on every run and every machine,
 * because a screenshot baseline is worthless if the source of truth
 * drifts. Three properties protect that:
 *
 * <ul>
 *   <li>no clock, frame counter, or animation phase is read;</li>
 *   <li>no pseudo-random generator is used — the waveform and the
 *       automation curve come from closed-form harmonic series;</li>
 *   <li>every transcendental call goes through {@link StrictMath}, whose
 *       results are bit-exact by specification. {@code Math.sin} is only
 *       required to be within one ulp, so it is permitted to differ
 *       between JDK builds and CPU architectures and cannot back a
 *       cross-platform baseline.</li>
 * </ul>
 *
 * <p>Text is drawn only when explicitly requested and is never part of a
 * baseline: glyph rasterization depends on the fonts installed on the
 * host. Text is measured separately by {@link TextShapingReport}.
 */
public final class RasterScene {
    /** Baseline geometry is authored at this logical size. */
    public static final int LOGICAL_WIDTH = 1440;

    /** Baseline geometry is authored at this logical height. */
    public static final int LOGICAL_HEIGHT = 900;

    private static final int TRACKS = 8;
    private static final int MIXER_STRIPS = 8;
    private static final int WAVEFORM_COLUMNS = 240;
    private static final int AUTOMATION_SEGMENTS = 6;
    private static final int METER_SEGMENTS = 16;

    private static final float TRANSPORT_HEIGHT = 64.0f;
    private static final float RULER_TOP = 72.0f;
    private static final float RULER_HEIGHT = 24.0f;
    private static final float LANE_TOP = 108.0f;
    private static final float LANE_HEIGHT = 62.0f;
    private static final float LANE_GAP = 6.0f;
    private static final float HEADER_WIDTH = 168.0f;
    private static final float MIXER_TOP = 664.0f;
    private static final float MIXER_HEIGHT = 220.0f;

    private static final int BACKGROUND = (int) 0xFF0E1013L;
    private static final int PANEL = (int) 0xFF181C23L;
    private static final int PANEL_EDGE = (int) 0xFF343B47L;
    private static final int ACCENT = (int) 0xFF3BAE91L;
    private static final int ACCENT_DIM = (int) 0xFF1F5B4EL;
    private static final int CLIP = (int) 0xFF4F61BFL;
    private static final int WAVEFORM = (int) 0xFFBFD0FFL;
    private static final int AUTOMATION = (int) 0xFFE0A24AL;
    private static final int TEXT = (int) 0xFFDDE3ECL;
    private static final int METER_HOT = (int) 0xFFD05A4EL;

    private RasterScene() {}

    /**
     * Draws the whole scene in logical coordinates. The caller applies
     * the display scale to {@code canvas} before calling, so the same
     * geometry is exercised at every scale factor.
     */
    public static void render(Canvas canvas, Font font) {
        canvas.clear(BACKGROUND);
        try (
            var fill = new Paint();
            var stroke = new Paint()
        ) {
            fill.setAntiAlias(true);
            fill.setMode(PaintMode.FILL);
            stroke.setAntiAlias(true);
            stroke.setMode(PaintMode.STROKE);
            stroke.setStrokeCap(PaintStrokeCap.ROUND);

            drawTransport(canvas, fill, stroke);
            drawRuler(canvas, fill);
            drawLanes(canvas, fill, stroke);
            drawMixer(canvas, fill, stroke);
            if (font != null) {
                drawText(canvas, fill, font);
            }
        }
    }

    private static void drawTransport(
        Canvas canvas,
        Paint fill,
        Paint stroke
    ) {
        fill.setColor(PANEL);
        canvas.drawRRect(
            RRect.Companion.makeXYWH(
                16.0f,
                12.0f,
                LOGICAL_WIDTH - 32.0f,
                TRANSPORT_HEIGHT - 24.0f,
                8.0f
            ),
            fill
        );
        stroke.setColor(PANEL_EDGE);
        stroke.setStrokeWidth(1.0f);
        canvas.drawRRect(
            RRect.Companion.makeXYWH(
                16.5f,
                12.5f,
                LOGICAL_WIDTH - 33.0f,
                TRANSPORT_HEIGHT - 25.0f,
                8.0f
            ),
            stroke
        );

        // Round transport buttons: circles are the cheapest way to make
        // any anti-aliasing or scaling regression visible, because a
        // half-pixel shift changes the edge coverage of every quadrant.
        for (var index = 0; index < 5; ++index) {
            var centerX = 48.0f + index * 44.0f;
            fill.setColor(index == 2 ? ACCENT : PANEL_EDGE);
            canvas.drawCircle(centerX, 32.0f, 13.0f, fill);
            stroke.setColor(ACCENT_DIM);
            stroke.setStrokeWidth(1.5f);
            canvas.drawCircle(centerX, 32.0f, 17.0f, stroke);
        }

        // Knob arcs, drawn as stroked partial circles via a path so the
        // scene exercises stroking of curved geometry, not just fills.
        for (var index = 0; index < 4; ++index) {
            var centerX = LOGICAL_WIDTH - 320.0f + index * 72.0f;
            var value = 0.15f + index * 0.22f;
            stroke.setColor(PANEL_EDGE);
            stroke.setStrokeWidth(4.0f);
            try (var track = arc(centerX, 32.0f, 16.0f, 1.0f)) {
                canvas.drawPath(track, stroke);
            }
            stroke.setColor(ACCENT);
            try (var filled = arc(centerX, 32.0f, 16.0f, value)) {
                canvas.drawPath(filled, stroke);
            }
        }
    }

    private static void drawRuler(Canvas canvas, Paint fill) {
        fill.setColor(PANEL);
        canvas.drawRect(
            HEADER_WIDTH,
            RULER_TOP,
            LOGICAL_WIDTH - 16.0f,
            RULER_TOP + RULER_HEIGHT,
            fill
        );
        var span = LOGICAL_WIDTH - 16.0f - HEADER_WIDTH;
        for (var bar = 0; bar <= 32; ++bar) {
            var x = HEADER_WIDTH + span * bar / 32.0f;
            var major = bar % 4 == 0;
            fill.setColor(major ? TEXT : PANEL_EDGE);
            canvas.drawRect(
                x,
                RULER_TOP + (major ? 4.0f : 12.0f),
                x + 1.0f,
                RULER_TOP + RULER_HEIGHT - 2.0f,
                fill
            );
        }
    }

    private static void drawLanes(
        Canvas canvas,
        Paint fill,
        Paint stroke
    ) {
        var clipLeft = HEADER_WIDTH + 8.0f;
        var clipWidth = LOGICAL_WIDTH - 24.0f - clipLeft;
        for (var track = 0; track < TRACKS; ++track) {
            var top = LANE_TOP + track * (LANE_HEIGHT + LANE_GAP);

            fill.setColor(PANEL);
            canvas.drawRRect(
                RRect.Companion.makeXYWH(
                    16.0f,
                    top,
                    HEADER_WIDTH - 24.0f,
                    LANE_HEIGHT,
                    6.0f
                ),
                fill
            );
            fill.setColor(track % 2 == 0 ? ACCENT : CLIP);
            canvas.drawRect(
                16.0f,
                top,
                20.0f,
                top + LANE_HEIGHT,
                fill
            );

            fill.setColor(CLIP);
            canvas.drawRRect(
                RRect.Companion.makeXYWH(
                    clipLeft,
                    top,
                    clipWidth,
                    LANE_HEIGHT,
                    4.0f
                ),
                fill
            );

            fill.setColor(WAVEFORM);
            try (var wave = waveformPath(
                track,
                clipLeft,
                clipWidth,
                top
            )) {
                canvas.drawPath(wave, fill);
            }

            stroke.setColor(AUTOMATION);
            stroke.setStrokeWidth(2.0f);
            try (var curve = automationPath(
                track,
                clipLeft,
                clipWidth,
                top
            )) {
                canvas.drawPath(curve, stroke);
            }
        }
    }

    private static void drawMixer(
        Canvas canvas,
        Paint fill,
        Paint stroke
    ) {
        var stripWidth = (LOGICAL_WIDTH - 32.0f) / MIXER_STRIPS;
        for (var strip = 0; strip < MIXER_STRIPS; ++strip) {
            var left = 16.0f + strip * stripWidth;
            fill.setColor(PANEL);
            canvas.drawRRect(
                RRect.Companion.makeXYWH(
                    left + 4.0f,
                    MIXER_TOP,
                    stripWidth - 8.0f,
                    MIXER_HEIGHT,
                    8.0f
                ),
                fill
            );

            // Fader track and cap.
            var faderX = left + stripWidth * 0.32f;
            fill.setColor(PANEL_EDGE);
            canvas.drawRRect(
                RRect.Companion.makeXYWH(
                    faderX - 3.0f,
                    MIXER_TOP + 24.0f,
                    6.0f,
                    MIXER_HEIGHT - 56.0f,
                    3.0f
                ),
                fill
            );
            var travel = MIXER_HEIGHT - 56.0f - 18.0f;
            var position = level(strip);
            fill.setColor(ACCENT);
            canvas.drawRRect(
                RRect.Companion.makeXYWH(
                    faderX - 13.0f,
                    MIXER_TOP + 24.0f + travel * (1.0f - position),
                    26.0f,
                    18.0f,
                    4.0f
                ),
                fill
            );

            // Segmented meter: a fixed number of lit segments per strip
            // so the capture has a countable, inspectable feature.
            var meterX = left + stripWidth * 0.66f;
            var segmentHeight =
                (MIXER_HEIGHT - 56.0f) / METER_SEGMENTS;
            var lit = (int) (position * METER_SEGMENTS);
            for (var segment = 0; segment < METER_SEGMENTS; ++segment) {
                var segmentTop = MIXER_TOP
                    + 24.0f
                    + (METER_SEGMENTS - 1 - segment) * segmentHeight;
                var on = segment < lit;
                if (!on) {
                    fill.setColor(PANEL_EDGE);
                } else if (segment >= METER_SEGMENTS - 3) {
                    fill.setColor(METER_HOT);
                } else {
                    fill.setColor(ACCENT);
                }
                canvas.drawRect(
                    meterX,
                    segmentTop + 1.0f,
                    meterX + 14.0f,
                    segmentTop + segmentHeight - 1.0f,
                    fill
                );
            }

            stroke.setColor(PANEL_EDGE);
            stroke.setStrokeWidth(1.0f);
            canvas.drawRRect(
                RRect.Companion.makeXYWH(
                    left + 4.5f,
                    MIXER_TOP + 0.5f,
                    stripWidth - 9.0f,
                    MIXER_HEIGHT - 1.0f,
                    8.0f
                ),
                stroke
            );
        }
    }

    private static void drawText(Canvas canvas, Paint fill, Font font) {
        fill.setColor(TEXT);
        for (var track = 0; track < TRACKS; ++track) {
            var top = LANE_TOP + track * (LANE_HEIGHT + LANE_GAP);
            try (var line = TextLine.Companion.make(
                "Track " + (track + 1),
                font
            )) {
                canvas.drawTextLine(line, 30.0f, top + 22.0f, fill);
            }
        }
    }

    private static Path waveformPath(
        int track,
        float left,
        float width,
        float top
    ) {
        var centerY = top + LANE_HEIGHT * 0.5f;
        var halfHeight = LANE_HEIGHT * 0.36f;
        try (var builder = new PathBuilder()) {
            for (var column = 0; column <= WAVEFORM_COLUMNS; ++column) {
                var t = column / (float) WAVEFORM_COLUMNS;
                var x = left + width * t;
                var y = centerY - halfHeight * envelope(track, t);
                if (column == 0) {
                    builder.moveTo(x, y);
                } else {
                    builder.lineTo(x, y);
                }
            }
            for (
                var column = WAVEFORM_COLUMNS;
                column >= 0;
                --column
            ) {
                var t = column / (float) WAVEFORM_COLUMNS;
                var x = left + width * t;
                var y = centerY + halfHeight * envelope(track, t);
                builder.lineTo(x, y);
            }
            builder.closePath();
            return builder.detach();
        }
    }

    private static Path automationPath(
        int track,
        float left,
        float width,
        float top
    ) {
        try (var builder = new PathBuilder()) {
            var startY = top + LANE_HEIGHT * (0.5f - 0.3f
                * curveValue(track, 0.0f));
            builder.moveTo(left + 4.0f, startY);
            for (
                var segment = 0;
                segment < AUTOMATION_SEGMENTS;
                ++segment
            ) {
                var t0 = segment / (float) AUTOMATION_SEGMENTS;
                var t1 = (segment + 1) / (float) AUTOMATION_SEGMENTS;
                var span = (t1 - t0) * (width - 8.0f);
                var x0 = left + 4.0f + t0 * (width - 8.0f);
                builder.cubicTo(
                    x0 + span * 0.35f,
                    top + LANE_HEIGHT * (0.5f - 0.3f
                        * curveValue(track, t0 + 0.05f)),
                    x0 + span * 0.65f,
                    top + LANE_HEIGHT * (0.5f - 0.3f
                        * curveValue(track, t1 - 0.05f)),
                    x0 + span,
                    top + LANE_HEIGHT * (0.5f - 0.3f
                        * curveValue(track, t1))
                );
            }
            return builder.detach();
        }
    }

    private static Path arc(
        float centerX,
        float centerY,
        float radius,
        float fraction
    ) {
        // Approximated with straight segments rather than Skia's arc
        // helpers: a fixed segment count keeps the tessellation identical
        // everywhere, whereas an arc's flattening tolerance is an
        // implementation detail we do not want inside a baseline.
        var segments = 48;
        var sweep = 2.0 * StrictMath.PI * 0.75 * fraction;
        var start = StrictMath.PI * 0.75;
        try (var builder = new PathBuilder()) {
            for (var index = 0; index <= segments; ++index) {
                var angle = start + sweep * index / segments;
                var x = centerX
                    + radius * (float) StrictMath.cos(angle);
                var y = centerY
                    + radius * (float) StrictMath.sin(angle);
                if (index == 0) {
                    builder.moveTo(x, y);
                } else {
                    builder.lineTo(x, y);
                }
            }
            return builder.detach();
        }
    }

    private static float envelope(int track, float t) {
        var phase = t * 2.0 * StrictMath.PI;
        var first = StrictMath.sin(phase * (4.0 + track * 0.5));
        var second = StrictMath.sin(phase * (11.0 + track) + track);
        var third = StrictMath.sin(phase * 37.0 + track * 2.0);
        var body = 0.58 * first + 0.27 * second + 0.15 * third;
        var window = 0.35 + 0.65 * StrictMath.sin(t * StrictMath.PI);
        return (float) StrictMath.abs(body * window);
    }

    private static float curveValue(int track, float t) {
        var phase = t * 2.0 * StrictMath.PI;
        return (float) StrictMath.sin(
            phase * (1.0 + track * 0.25) + track * 0.5
        );
    }

    private static float level(int strip) {
        return (float) (0.5
            + 0.45 * StrictMath.sin(strip * 0.8 + 0.3));
    }
}
