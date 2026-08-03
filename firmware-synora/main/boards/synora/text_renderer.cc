#include "text_renderer.h"

#include <cstring>

namespace {

// Appends up to (dest_size - 1 - dest_len) bytes of src to dest[dest_len...],
// keeps dest null-terminated, and returns the new length -- plain bounded
// memcpy rather than snprintf's "%.*s", since GCC's format-truncation
// checker can't prove src_len is bounded and flags that pattern as a
// possible overflow even though it never is here.
size_t AppendBounded(char* dest, size_t dest_size, size_t dest_len, const char* src, size_t src_len) {
    size_t space = dest_size - 1 - dest_len;
    size_t n = src_len < space ? src_len : space;
    std::memcpy(dest + dest_len, src, n);
    dest_len += n;
    dest[dest_len] = '\0';
    return dest_len;
}

// Decodes one UTF-8 codepoint starting at *cursor and advances *cursor past
// it. A malformed/truncated sequence decodes as U+FFFD and advances by one
// byte -- never advances by zero bytes, which would infinite-loop the caller.
uint32_t DecodeUtf8(const char** cursor) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(*cursor);
    uint8_t lead = p[0];

    if (lead < 0x80) {
        *cursor += 1;
        return lead;
    }

    int extra_bytes;
    uint32_t codepoint;
    if ((lead & 0xE0) == 0xC0) {
        extra_bytes = 1;
        codepoint = lead & 0x1F;
    } else if ((lead & 0xF0) == 0xE0) {
        extra_bytes = 2;
        codepoint = lead & 0x0F;
    } else if ((lead & 0xF8) == 0xF0) {
        extra_bytes = 3;
        codepoint = lead & 0x07;
    } else {
        *cursor += 1;
        return 0xFFFD;
    }

    for (int i = 1; i <= extra_bytes; i++) {
        uint8_t b = p[i];
        if (b == 0 || (b & 0xC0) != 0x80) {
            // Truncated string or malformed continuation byte -- bail after
            // just the lead byte rather than reading past the end.
            *cursor += 1;
            return 0xFFFD;
        }
        codepoint = (codepoint << 6) | (b & 0x3F);
    }
    *cursor += 1 + extra_bytes;
    return codepoint;
}

const GlyphBitmap* FindGlyph(uint32_t codepoint, const GlyphBitmap* glyphs, size_t glyph_count) {
    size_t lo = 0;
    size_t hi = glyph_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (glyphs[mid].codepoint < codepoint) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo < glyph_count && glyphs[lo].codepoint == codepoint) {
        return &glyphs[lo];
    }
    return nullptr;
}

void BlitGlyph(uint16_t* framebuffer, int fb_width, int fb_height, int pen_x, int pen_y, const GlyphBitmap& glyph,
               uint16_t color) {
    if (glyph.bitmap == nullptr) {
        return;
    }
    int row_bytes = (glyph.width + 7) / 8;
    for (int row = 0; row < glyph.height; row++) {
        int py = pen_y + glyph.y_offset + row;
        if (py < 0 || py >= fb_height) {
            continue;
        }
        const uint8_t* row_data = glyph.bitmap + row * row_bytes;
        for (int col = 0; col < glyph.width; col++) {
            int px = pen_x + glyph.x_offset + col;
            if (px < 0 || px >= fb_width) {
                continue;
            }
            uint8_t byte = row_data[col / 8];
            bool set = (byte >> (7 - (col % 8))) & 1;
            if (set) {
                framebuffer[py * fb_width + px] = color;
            }
        }
    }
}

}  // namespace

int DrawText(uint16_t* framebuffer, int fb_width, int fb_height, int x, int y, const char* utf8_text, uint16_t color,
             const GlyphBitmap* glyphs, size_t glyph_count, uint8_t line_height) {
    int pen_x = x;
    const char* cursor = utf8_text;
    while (*cursor != '\0') {
        uint32_t codepoint = DecodeUtf8(&cursor);
        const GlyphBitmap* glyph = FindGlyph(codepoint, glyphs, glyph_count);
        if (glyph == nullptr) {
            pen_x += line_height / 2;
            continue;
        }
        BlitGlyph(framebuffer, fb_width, fb_height, pen_x, y, *glyph, color);
        pen_x += glyph->advance;
    }
    return pen_x - x;
}

int MeasureTextWidth(const char* utf8_text, const GlyphBitmap* glyphs, size_t glyph_count, uint8_t line_height) {
    int width = 0;
    const char* cursor = utf8_text;
    while (*cursor != '\0') {
        uint32_t codepoint = DecodeUtf8(&cursor);
        const GlyphBitmap* glyph = FindGlyph(codepoint, glyphs, glyph_count);
        if (glyph == nullptr) {
            width += line_height / 2;
            continue;
        }
        width += glyph->advance;
    }
    return width;
}

int DrawWrappedText(uint16_t* framebuffer, int fb_width, int fb_height, int x, int y, const char* utf8_text,
                     uint16_t color, const GlyphBitmap* glyphs, size_t glyph_count, uint8_t line_height) {
    constexpr size_t kLineBufSize = 256;
    char line[kLineBufSize] = "";
    char trial[kLineBufSize];
    size_t line_len = 0;
    int pen_y = y;
    const char* cursor = utf8_text;

    while (true) {
        while (*cursor == ' ') {
            cursor++;
        }
        if (*cursor == '\0') {
            break;
        }
        const char* word_start = cursor;
        while (*cursor != '\0' && *cursor != ' ') {
            cursor++;
        }
        size_t word_len = static_cast<size_t>(cursor - word_start);

        size_t trial_len = 0;
        if (line_len > 0) {
            trial_len = AppendBounded(trial, sizeof(trial), trial_len, line, line_len);
            trial_len = AppendBounded(trial, sizeof(trial), trial_len, " ", 1);
        }
        trial_len = AppendBounded(trial, sizeof(trial), trial_len, word_start, word_len);

        if (line_len > 0 && MeasureTextWidth(trial, glyphs, glyph_count, line_height) > fb_width - x) {
            DrawText(framebuffer, fb_width, fb_height, x, pen_y, line, color, glyphs, glyph_count, line_height);
            pen_y += line_height;
            if (pen_y >= fb_height) {
                return pen_y;
            }
            line_len = AppendBounded(line, sizeof(line), 0, word_start, word_len);
        } else {
            line_len = AppendBounded(line, sizeof(line), 0, trial, trial_len);
        }
    }

    if (line_len > 0) {
        DrawText(framebuffer, fb_width, fb_height, x, pen_y, line, color, glyphs, glyph_count, line_height);
        pen_y += line_height;
    }
    return pen_y;
}
