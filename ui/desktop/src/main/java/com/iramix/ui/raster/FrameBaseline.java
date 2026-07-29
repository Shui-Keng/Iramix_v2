package com.iramix.ui.raster;

import java.awt.image.BufferedImage;
import java.io.IOException;
import java.io.UncheckedIOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.HexFormat;
import javax.imageio.ImageIO;

/**
 * Reads and writes the committed screenshot baselines.
 *
 * <p>Baselines are stored as opaque PNG through {@code ImageIO} rather
 * than through Skia's own encoder. The point of a baseline is to detect
 * a change in Skia's output, so the storage format must not itself be a
 * Skia artefact — otherwise a codec change could mask or manufacture a
 * difference. {@code TYPE_INT_RGB} PNG is lossless, so the stored file
 * and the compared pixels are the same values.
 */
public final class FrameBaseline {
    private FrameBaseline() {}

    /** A decoded baseline: dimensions plus packed {@code 0xRRGGBB}. */
    public record Frame(int width, int height, int[] rgb) {}

    /** How one capture differs from its baseline. */
    public record Difference(
        long differingPixels,
        int maxChannelDelta
    ) {
        public boolean identical() {
            return differingPixels == 0L;
        }
    }

    public static void write(
        Path file,
        int width,
        int height,
        int[] rgb
    ) {
        var image = new BufferedImage(
            width,
            height,
            BufferedImage.TYPE_INT_RGB
        );
        image.setRGB(0, 0, width, height, rgb, 0, width);
        try {
            var parent = file.getParent();
            if (parent != null) {
                Files.createDirectories(parent);
            }
            if (!ImageIO.write(image, "png", file.toFile())) {
                throw new IllegalStateException(
                    "No PNG writer was available for " + file + "."
                );
            }
        } catch (IOException exception) {
            throw new UncheckedIOException(exception);
        }
    }

    public static Frame read(Path file) {
        BufferedImage image;
        try {
            image = ImageIO.read(file.toFile());
        } catch (IOException exception) {
            throw new UncheckedIOException(exception);
        }
        if (image == null) {
            throw new IllegalStateException(
                "Baseline " + file + " could not be decoded as PNG."
            );
        }
        var width = image.getWidth();
        var height = image.getHeight();
        var rgb = new int[width * height];
        image.getRGB(0, 0, width, height, rgb, 0, width);
        for (var index = 0; index < rgb.length; ++index) {
            rgb[index] &= 0x00FFFFFF;
        }
        return new Frame(width, height, rgb);
    }

    /**
     * Compares a capture against a baseline.
     *
     * <p>A dimension mismatch is reported as a hard failure rather than
     * as a large pixel difference: it means the scale matrix itself
     * changed, which no per-pixel tolerance should ever absorb.
     */
    public static Difference compare(
        Frame baseline,
        int width,
        int height,
        int[] rgb
    ) {
        if (baseline.width() != width || baseline.height() != height) {
            throw new IllegalStateException(
                "Baseline is " + baseline.width() + "x"
                    + baseline.height() + " but the capture is "
                    + width + "x" + height + "."
            );
        }
        var differing = 0L;
        var maxDelta = 0;
        var expected = baseline.rgb();
        for (var index = 0; index < rgb.length; ++index) {
            var actualPixel = rgb[index];
            var expectedPixel = expected[index];
            if (actualPixel == expectedPixel) {
                continue;
            }
            ++differing;
            for (var shift = 0; shift <= 16; shift += 8) {
                var delta = Math.abs(
                    ((actualPixel >> shift) & 0xFF)
                        - ((expectedPixel >> shift) & 0xFF)
                );
                maxDelta = Math.max(maxDelta, delta);
            }
        }
        return new Difference(differing, maxDelta);
    }

    /**
     * A SHA-256 over the packed RGB bytes, in big-endian channel order.
     *
     * <p>This is the token quoted in the result document: it identifies
     * a capture exactly without committing the reader to open a PNG.
     */
    public static String digest(int[] rgb) {
        MessageDigest digest;
        try {
            digest = MessageDigest.getInstance("SHA-256");
        } catch (NoSuchAlgorithmException exception) {
            throw new IllegalStateException(
                "SHA-256 is required but unavailable.",
                exception
            );
        }
        var buffer = new byte[rgb.length * 3];
        for (var index = 0; index < rgb.length; ++index) {
            var pixel = rgb[index];
            buffer[index * 3] = (byte) (pixel >> 16);
            buffer[index * 3 + 1] = (byte) (pixel >> 8);
            buffer[index * 3 + 2] = (byte) pixel;
        }
        return HexFormat.of().formatHex(digest.digest(buffer));
    }
}
