package com.iramix.ui.raster;

import org.jetbrains.skia.Bitmap;
import org.jetbrains.skia.ColorAlphaType;
import org.jetbrains.skia.ColorType;
import org.jetbrains.skia.Font;
import org.jetbrains.skia.ImageInfo;
import org.jetbrains.skia.Surface;

/**
 * One raster capture of {@link RasterScene} at a given display scale.
 *
 * <p>The surface itself is N32 premultiplied, which is BGRA on Windows
 * and RGBA elsewhere. Pixels are therefore never compared in surface
 * order: {@link #readRgb()} always reads back through an explicit
 * {@link ColorType#RGBA_8888} image info, so a baseline captured on one
 * operating system is byte-comparable against a capture from another.
 * Skipping that conversion would produce channel-swapped "differences"
 * that look like a rendering regression and are not one.
 */
public final class RasterFrame implements AutoCloseable {
    private final float scale;
    private final int widthPixels;
    private final int heightPixels;
    private final Surface surface;

    public RasterFrame(float scale) {
        this.scale = scale;
        this.widthPixels = pixels(RasterScene.LOGICAL_WIDTH, scale);
        this.heightPixels = pixels(RasterScene.LOGICAL_HEIGHT, scale);
        this.surface = Surface.Companion.makeRasterN32Premul(
            widthPixels,
            heightPixels
        );
    }

    public float scale() {
        return scale;
    }

    public int widthPixels() {
        return widthPixels;
    }

    public int heightPixels() {
        return heightPixels;
    }

    /**
     * Draws the scene once. {@code font} may be null, which is what a
     * baseline capture uses — see {@link RasterScene}.
     */
    public void render(Font font) {
        var canvas = surface.getCanvas();
        var count = canvas.save();
        canvas.scale(scale, scale);
        RasterScene.render(canvas, font);
        canvas.restoreToCount(count);
        surface.flush();
    }

    /**
     * Reads the frame back as packed {@code 0xRRGGBB} values.
     *
     * <p>The alpha channel is dropped rather than compared: the scene
     * clears to an opaque colour, so alpha carries no information, and
     * dropping it lets the baseline be an ordinary opaque PNG that a
     * human can open and inspect.
     */
    public int[] readRgb() {
        var info = ImageInfo.Companion.makeN32Premul(
            widthPixels,
            heightPixels
        )
            .withColorType(ColorType.RGBA_8888)
            .withColorAlphaType(ColorAlphaType.PREMUL);
        try (var bitmap = new Bitmap()) {
            if (!bitmap.allocPixels(info)) {
                throw new IllegalStateException(
                    "Could not allocate a readback bitmap for "
                        + widthPixels + "x" + heightPixels + "."
                );
            }
            if (!surface.readPixels(bitmap, 0, 0)) {
                throw new IllegalStateException(
                    "Surface readback failed at scale " + scale + "."
                );
            }
            var rowBytes = info.getMinRowBytes();
            var bytes = bitmap.readPixels(info, rowBytes, 0, 0);
            if (bytes == null) {
                throw new IllegalStateException(
                    "Bitmap readback returned no pixels at scale "
                        + scale + "."
                );
            }
            return toRgb(bytes);
        }
    }

    @Override
    public void close() {
        surface.close();
    }

    private static int[] toRgb(byte[] rgba) {
        var pixels = new int[rgba.length / 4];
        for (var index = 0; index < pixels.length; ++index) {
            var offset = index * 4;
            var alpha = rgba[offset + 3] & 0xFF;
            if (alpha != 0xFF) {
                throw new IllegalStateException(
                    "The reference scene must be fully opaque; found "
                        + "alpha " + alpha + " at pixel " + index + "."
                );
            }
            pixels[index] = ((rgba[offset] & 0xFF) << 16)
                | ((rgba[offset + 1] & 0xFF) << 8)
                | (rgba[offset + 2] & 0xFF);
        }
        return pixels;
    }

    private static int pixels(int logical, float scale) {
        return Math.round(logical * scale);
    }
}
