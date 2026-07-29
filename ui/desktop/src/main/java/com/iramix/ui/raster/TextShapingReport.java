package com.iramix.ui.raster;

import java.util.ArrayList;
import java.util.List;
import org.jetbrains.skia.Font;
import org.jetbrains.skia.FontEdging;
import org.jetbrains.skia.FontHinting;
import org.jetbrains.skia.FontMgr;
import org.jetbrains.skia.FontStyle;
import org.jetbrains.skia.TextLine;
import org.jetbrains.skia.Typeface;

/**
 * Measures what the host actually does with text, rather than asserting
 * it.
 *
 * <p>Text is deliberately excluded from the screenshot baselines: glyph
 * outlines and hinting come from fonts installed on the machine, so a
 * pixel baseline for text would encode this workstation's font set and
 * fail everywhere else for reasons that are not regressions. What can be
 * carried across machines is the *behaviour*: which family the font
 * manager resolves, how many glyphs a string shapes to, and whether the
 * primary face covers the script at all. That is what this report emits.
 *
 * <p>Two distinct facts are recorded per sample, and they are not
 * interchangeable:
 *
 * <ul>
 *   <li>{@code missingGlyphs} — how many codepoints the *primary* face
 *       maps to glyph 0 ({@code .notdef}). A non-zero count means the UI
 *       font alone cannot render the string.</li>
 *   <li>{@code fallbackFamily} — the family the font manager substitutes
 *       for the sample's first codepoint. This is the mechanism that
 *       would have to be driven explicitly, because
 *       {@link TextLine#make} shapes with the single font it is given
 *       and does not consult the manager.</li>
 * </ul>
 */
public final class TextShapingReport {
    /**
     * Families tried in order for the UI face. The list is deliberately
     * per-platform-ish rather than a single name: resolving to *some*
     * face on every host is what keeps the report runnable in CI.
     */
    private static final String[] UI_FAMILIES = {
        "Inter",
        "Segoe UI",
        "Helvetica Neue",
        "DejaVu Sans",
        "Noto Sans",
        "Arial",
    };

    private static final float SIZE = 14.0f;

    private TextShapingReport() {}

    /** One measured string. */
    public record Sample(
        String label,
        String script,
        int codepoints,
        int shapedGlyphs,
        int missingGlyphs,
        float width,
        String fallbackFamily
    ) {}

    /** The whole report for one host. */
    public record Report(
        String resolvedFamily,
        int typefaceGlyphs,
        int familiesAvailable,
        List<Sample> samples
    ) {}

    /**
     * Builds the UI font using the same rasterization settings the
     * scene uses, so a later comparison against on-screen text is
     * meaningful.
     */
    public static Font makeUiFont() {
        var typeface = resolveTypeface();
        var font = new Font(typeface, SIZE);
        font.setEdging(FontEdging.ANTI_ALIAS);
        font.setHinting(FontHinting.SLIGHT);
        font.setSubpixel(true);
        return font;
    }

    public static Report measure() {
        var manager = FontMgr.Companion.getDefault();
        var typeface = resolveTypeface();
        try (var font = new Font(typeface, SIZE)) {
            font.setEdging(FontEdging.ANTI_ALIAS);
            font.setHinting(FontHinting.SLIGHT);
            font.setSubpixel(true);

            var samples = new ArrayList<Sample>();
            samples.add(measure(
                manager,
                typeface,
                font,
                "ascii",
                "Latn",
                "en",
                "Track 1 - Bus A 0.0 dB"
            ));
            samples.add(measure(
                manager,
                typeface,
                font,
                "latin-diacritics",
                "Latn",
                "de",
                // Non-ASCII samples are written as escapes, not as
                // literals: the codepoints under test are the whole
                // point of the sample, and an escape cannot be silently
                // re-encoded by an editor or a tool that guesses the
                // file encoding. U+00DC, U+00EF, U+2014, U+00E9.
                "Übergang naïve — café"
            ));
            samples.add(measure(
                manager,
                typeface,
                font,
                "cjk",
                "Jpan",
                "ja",
                "音量オート"
            ));
            samples.add(measure(
                manager,
                typeface,
                font,
                "rtl-arabic",
                "Arab",
                "ar",
                "مسار الص"
                    + "وت"
            ));
            samples.add(measure(
                manager,
                typeface,
                font,
                "emoji-astral",
                "Zsye",
                "en",
                // U+1F39B and U+1F39A are astral, so they are surrogate
                // pairs in Java and the codepoint count must not be read
                // off String.length().
                "🎛🎚"
            ));

            return new Report(
                typeface.getFamilyName(),
                typeface.getGlyphsCount(),
                manager.getFamiliesCount(),
                List.copyOf(samples)
            );
        }
    }

    private static Sample measure(
        FontMgr manager,
        Typeface typeface,
        Font font,
        String label,
        String script,
        String language,
        String text
    ) {
        var glyphs = font.getStringGlyphs(text);
        var missing = 0;
        for (var glyph : glyphs) {
            if (glyph == 0) {
                ++missing;
            }
        }
        try (var line = TextLine.Companion.make(text, font)) {
            return new Sample(
                label,
                script,
                text.codePointCount(0, text.length()),
                line.getGlyphs().length,
                missing,
                line.getWidth(),
                fallbackFamily(
                    manager,
                    typeface,
                    language,
                    text.codePointAt(0)
                )
            );
        }
    }

    private static String fallbackFamily(
        FontMgr manager,
        Typeface typeface,
        String language,
        int codepoint
    ) {
        var match = manager.matchFamilyStyleCharacter(
            typeface.getFamilyName(),
            FontStyle.Companion.getNORMAL(),
            new String[] { language },
            codepoint
        );
        if (match == null) {
            return "none";
        }
        try {
            return match.getFamilyName();
        } finally {
            match.close();
        }
    }

    private static Typeface resolveTypeface() {
        var manager = FontMgr.Companion.getDefault();
        for (var family : UI_FAMILIES) {
            var typeface = manager.matchFamilyStyle(
                family,
                FontStyle.Companion.getNORMAL()
            );
            if (typeface != null) {
                return typeface;
            }
        }
        // Falling back to the empty family asks the manager for whatever
        // it considers default. If that is also absent the host has no
        // usable fonts, which is a finding, not something to paper over.
        var fallback = manager.matchFamilyStyle(
            "",
            FontStyle.Companion.getNORMAL()
        );
        if (fallback == null) {
            throw new IllegalStateException(
                "No font family could be resolved on this host."
            );
        }
        return fallback;
    }
}
