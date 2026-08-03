#include "test_util.h"

#include "../main/boards/synora/text_renderer.h"

#include <vector>

namespace {

std::vector<uint16_t> MakeBlankFramebuffer(int width, int height) {
    return std::vector<uint16_t>(static_cast<size_t>(width) * height, 0);
}

// Tiny synthetic 3-glyph table for testing the renderer's decode/lookup/blit
// logic in isolation from the real (much larger) generated font data.
// Sorted ascending by codepoint, as DrawText/MeasureTextWidth require.
const uint8_t kGlyphA_bitmap[] = {0xF0, 0xF0, 0xF0, 0xF0};  // 'A': 4x4 filled square
const uint8_t kGlyphE_bitmap[] = {0xC0, 0xC0};              // 'é' (U+00E9): 2x2 filled square

const GlyphBitmap kGlyphs[] = {
    {0x0020, 0, 0, 0, 0, 2, nullptr},          // space: advance-only, no bitmap
    {0x0041, 4, 4, 0, -4, 5, kGlyphA_bitmap},  // 'A'
    {0x00E9, 2, 2, 0, -2, 3, kGlyphE_bitmap},  // 'é' -- exercises 2-byte UTF-8 decoding
};
constexpr size_t kGlyphCount = sizeof(kGlyphs) / sizeof(kGlyphs[0]);
constexpr uint8_t kLineHeight = 8;

}  // namespace

int RunTextRendererTests() {
    TEST_SUITE_BEGIN();

    // A single glyph draws at the expected position: pen baseline at y=10,
    // 'A' is 4x4 with y_offset=-4, so its rows land at py 6..9.
    {
        auto fb = MakeBlankFramebuffer(20, 20);
        int width = DrawText(fb.data(), 20, 20, 0, 10, "A", 0xFFFF, kGlyphs, kGlyphCount, kLineHeight);
        TEST_CHECK_EQ(width, 5);  // 'A's advance
        TEST_CHECK_EQ(fb[8 * 20 + 2], 0xFFFF);
        TEST_CHECK_EQ(fb[5 * 20 + 2], 0);   // row just above the glyph, untouched
        TEST_CHECK_EQ(fb[10 * 20 + 2], 0);  // baseline row itself, untouched
    }

    // Space is advance-only -- draws nothing, still moves the pen.
    {
        TEST_CHECK_EQ(MeasureTextWidth(" ", kGlyphs, kGlyphCount, kLineHeight), 2);
        auto fb = MakeBlankFramebuffer(20, 20);
        DrawText(fb.data(), 20, 20, 0, 10, " ", 0xFFFF, kGlyphs, kGlyphCount, kLineHeight);
        for (auto pixel : fb) {
            TEST_CHECK_EQ(pixel, 0);
        }
    }

    // Multi-byte UTF-8 decodes as one codepoint, not two mis-decoded bytes.
    {
        TEST_CHECK_EQ(MeasureTextWidth("\xC3\xA9", kGlyphs, kGlyphCount, kLineHeight), 3);
    }

    // A codepoint missing from the table draws nothing but still advances
    // by line_height/2 (matching MeasureTextWidth), and doesn't stop the
    // rest of the string from rendering.
    {
        TEST_CHECK_EQ(MeasureTextWidth("Z", kGlyphs, kGlyphCount, kLineHeight), kLineHeight / 2);

        auto fb = MakeBlankFramebuffer(20, 20);
        int width = DrawText(fb.data(), 20, 20, 0, 10, "ZA", 0xFFFF, kGlyphs, kGlyphCount, kLineHeight);
        TEST_CHECK_EQ(width, kLineHeight / 2 + 5);  // missing 'Z' fallback + 'A's advance
        TEST_CHECK_EQ(fb[8 * 20 + (kLineHeight / 2 + 2)], 0xFFFF);  // 'A' shifted right past the fallback gap
    }

    // A glyph straddling the framebuffer edge clips instead of writing out
    // of bounds.
    {
        auto fb = MakeBlankFramebuffer(6, 6);
        DrawText(fb.data(), 6, 6, 4, 3, "A", 0xFFFF, kGlyphs, kGlyphCount, kLineHeight);
        TEST_CHECK_EQ(fb.size(), static_cast<size_t>(6) * 6);  // no out-of-bounds resize/crash
        TEST_CHECK_EQ(fb[0 * 6 + 4], 0xFFFF);                  // in-bounds portion of the clipped glyph
    }

    // DrawWrappedText: a line breaks once the next word would overflow
    // fb_width, and the wrapped word starts the next line instead of being
    // dropped -- "A A" (each 'A' advances 5px) doesn't fit twice in an 8px
    // wide canvas starting at x=0, so it wraps to two lines one line_height
    // apart.
    {
        auto fb = MakeBlankFramebuffer(8, 20);
        int end_y = DrawWrappedText(fb.data(), 8, 20, 0, 10, "A A", 0xFFFF, kGlyphs, kGlyphCount, kLineHeight);
        TEST_CHECK_EQ(end_y, 10 + kLineHeight * 2);
        TEST_CHECK_EQ(fb[8 * 8 + 2], 0xFFFF);                  // first 'A', first line's baseline-relative row
        TEST_CHECK_EQ(fb[(8 + kLineHeight) * 8 + 2], 0xFFFF);  // second 'A', wrapped to its own line
    }

    // Running out of vertical space stops drawing further lines rather than
    // writing past fb_height: the very first line's flush already lands at
    // pen_y=18 (>= fb_height=12), so "A A A" never gets past its first word.
    {
        auto fb = MakeBlankFramebuffer(4, 12);
        int end_y = DrawWrappedText(fb.data(), 4, 12, 0, 10, "A A A", 0xFFFF, kGlyphs, kGlyphCount, kLineHeight);
        TEST_CHECK_EQ(end_y, 10 + kLineHeight);
    }

    TEST_SUITE_END();
}
