#!/usr/bin/env python3
"""Generates the Vietnamese bitmap font + download-QR-code assets baked into
main/boards/synora/font_noto_sans_vi.{h,cc} and qr_download.{h,cc}.

Rasterizes Noto Sans Regular at two fixed pixel sizes (24px for the pairing
screen, 18px for the sensor dashboard) into 1bpp glyph bitmaps consumed by
text_renderer.h's GlyphBitmap table, covering Basic Latin, Latin-1
Supplement, Latin Extended-A, and Latin Extended Additional -- enough for
full Vietnamese (including precomposed tone-marked vowels and Ð/đ) plus the
degree sign for "°C". Also renders a QR code for the device-setup download
page as a same-format 1bpp bitmap.

There's no on-device visual feedback loop for this (ST7793 bring-up this
session already showed how expensive iterating blind on real hardware is),
so --preview renders both planned screens to a PNG at the real 240x400
resolution using Pillow directly -- the only pre-hardware check available.

Usage:
    python3 gen_font.py            # writes the .h/.cc files
    python3 gen_font.py --preview  # also writes a PNG mockup of both screens
"""
import argparse
import pathlib
import sys
import unicodedata

from PIL import Image, ImageDraw, ImageFont

FONT_PATH = "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf"
OUTPUT_DIR = pathlib.Path(__file__).resolve().parent.parent / "main" / "boards" / "synora"

# Basic Latin, Latin-1 Supplement, Latin Extended-A, Latin Extended-B,
# Latin Extended Additional -- covers ASCII, degree sign (U+00B0, "°C"),
# Đ/đ (U+0110/0111), and essentially all Vietnamese precomposed
# tone-marked vowels. Extended-B (U+0180-024F) is easy to miss but
# essential: the bare "horn" letters Ơ/ơ/Ư/ư (U+01A0/01A1/01AF/01B0) live
# there, distinct from Extended Additional's TONE-MARKED horn vowels (e.g.
# ớ = U+1EDB) -- missing this range renders "ư" as a blank space while
# "ớ" works fine, which is exactly the bug that prompted adding this.
CODEPOINT_RANGES = [
    (0x0020, 0x007E),
    (0x00A0, 0x00FF),
    (0x0100, 0x017F),
    (0x0180, 0x024F),
    (0x1E00, 0x1EFF),
]

FONT_SIZES = {
    "24": 24,  # pairing screen (1-2 lines)
    "18": 18,  # sensor dashboard (4 lines)
    "14": 14,  # dashboard auto-water status bar -- smaller than the rest of
               # the dashboard's body text
    "64": 64,  # dashboard hero clock -- rasterized at native size rather than
               # upscaled, so digits/colon have real curve detail instead of
               # blocky nearest-neighbor edges.
}

DOWNLOAD_URL = "https://synora.id.vn/downloads/"

PAIRING_TITLE = "Dùng ứng dụng Synora để cài đặt thiết bị"
PAIRING_HINT = "Mở ứng dụng Synora trên điện thoại để ghép nối thiết bị"
DASHBOARD_LINES = [
    "Nước {}",
    "Nhiệt độ {}",
    "Độ ẩm {}",
    "Đất {}",
]

# The 12 Vietnamese vowel letters (bare + the 6 diacritic-modified ones --
# ă â ê ô ơ ư) crossed with the 6 tones (ngang/no mark, huyền, sắc, hỏi, ngã,
# nặng), upper- and lowercase, plus the consonants unique to the Vietnamese
# alphabet (đ has no tones of its own). Composed via unicodedata rather than
# hand-typed as literal precomposed characters -- typing ~180 accented
# characters by hand is exactly the error-prone case that produced the "ư"
# rendering bug this test string exists to catch a regression of; combining
# base+mark and letting NFC normalize to the precomposed codepoint is
# mechanically checkable instead of relying on getting every keystroke right.
_VOWELS = "aăâeêioôơuưy"
# Combining diacriticals by \u escape, not as literal combining characters in
# this file's source (which would be invisible/unverifiable in a diff) --
# ngang (no mark), huyen (grave, U+0300), sac (acute, U+0301),
# hoi (hook above, U+0309), nga (tilde, U+0303), nang (dot below, U+0323).
_TONE_MARKS = ["", "̀", "́", "̉", "̃", "̣"]
_EXTRA_CONSONANTS = "bcdđghklmnpqrstvx"


def vietnamese_test_string():
    groups = []
    for v in _VOWELS:
        forms = [unicodedata.normalize("NFC", v + m) for m in _TONE_MARKS]
        forms += [unicodedata.normalize("NFC", v.upper() + m) for m in _TONE_MARKS]
        groups.append("".join(forms))
    groups.append(_EXTRA_CONSONANTS + _EXTRA_CONSONANTS.upper())
    return " ".join(groups)


def codepoints():
    for lo, hi in CODEPOINT_RANGES:
        yield from range(lo, hi + 1)


def rasterize_glyph(font, ascent, codepoint):
    """Returns a dict of glyph metrics/bitmap bytes, or None if the font has
    no usable glyph for this codepoint (e.g. an unassigned slot)."""
    ch = chr(codepoint)
    bbox = font.getbbox(ch)
    if bbox is None:
        return None
    left, top, right, bottom = bbox
    width = right - left
    height = bottom - top
    advance = round(font.getlength(ch))

    if width <= 0 or height <= 0:
        # No ink (e.g. space) -- advance-only glyph, no bitmap.
        return {"codepoint": codepoint, "width": 0, "height": 0, "x_offset": 0, "y_offset": 0,
                "advance": advance, "bitmap": None}

    # top/bottom are relative to Pillow's default anchor (left, ascender-top,
    # not baseline) -- rebase to baseline so it matches text_renderer.h's
    # "y_offset is relative to the pen's baseline" convention.
    y_offset = top - ascent
    x_offset = left

    if x_offset < -128 or x_offset > 127 or y_offset < -128 or y_offset > 127:
        raise ValueError(f"U+{codepoint:04X} offset out of int8 range: x={x_offset} y={y_offset}")
    if width > 255 or height > 255 or advance > 255:
        raise ValueError(f"U+{codepoint:04X} dimension out of uint8 range: w={width} h={height} adv={advance}")

    img = Image.new("L", (width, height), 0)
    draw = ImageDraw.Draw(img)
    draw.text((-left, -top), ch, font=font, fill=255)
    pixels = img.load()

    row_bytes = (width + 7) // 8
    bitmap = bytearray(row_bytes * height)
    for y in range(height):
        for x in range(width):
            if pixels[x, y] >= 128:
                bitmap[y * row_bytes + x // 8] |= 0x80 >> (x % 8)

    return {"codepoint": codepoint, "width": width, "height": height, "x_offset": x_offset,
            "y_offset": y_offset, "advance": advance, "bitmap": bytes(bitmap)}


def rasterize_font(size_px):
    font = ImageFont.truetype(FONT_PATH, size_px)
    ascent, _descent = font.getmetrics()
    glyphs = []
    missing = []
    for cp in codepoints():
        glyph = rasterize_glyph(font, ascent, cp)
        if glyph is None:
            missing.append(cp)
        else:
            glyphs.append(glyph)
    return glyphs, missing


def emit_glyph_table_cc(out, size_name, glyphs):
    for g in glyphs:
        if g["bitmap"] is None:
            continue
        name = f"kGlyph{size_name}_{g['codepoint']:04X}"
        bytes_str = ", ".join(f"0x{b:02X}" for b in g["bitmap"])
        out.write(f"static const uint8_t {name}[] = {{{bytes_str}}};\n")

    out.write(f"\nconst GlyphBitmap kFontGlyphs{size_name}[] = {{\n")
    for g in glyphs:
        bitmap_ref = f"kGlyph{size_name}_{g['codepoint']:04X}" if g["bitmap"] is not None else "nullptr"
        out.write(f"    {{0x{g['codepoint']:04X}, {g['width']}, {g['height']}, {g['x_offset']}, "
                  f"{g['y_offset']}, {g['advance']}, {bitmap_ref}}},\n")
    out.write("};\n")
    out.write(f"const size_t kFontGlyphCount{size_name} = "
              f"sizeof(kFontGlyphs{size_name}) / sizeof(kFontGlyphs{size_name}[0]);\n\n")


def write_font_files(sizes_glyphs):
    header = OUTPUT_DIR / "font_noto_sans_vi.h"
    source = OUTPUT_DIR / "font_noto_sans_vi.cc"

    test_string = vietnamese_test_string()

    with header.open("w") as out:
        out.write("// GENERATED by scripts/gen_font.py -- do not hand-edit.\n")
        out.write("// Noto Sans Regular, rasterized at each size below, covering Basic Latin +\n")
        out.write("// Latin-1 Supplement + Latin Extended-A + Latin Extended Additional (full\n")
        out.write("// Vietnamese + the degree sign). Glyphs are sorted ascending by codepoint,\n")
        out.write("// as text_renderer.h's binary search requires.\n")
        out.write("#pragma once\n\n#include \"text_renderer.h\"\n\n")
        for size_name in sizes_glyphs:
            out.write(f"extern const GlyphBitmap kFontGlyphs{size_name}[];\n")
            out.write(f"extern const size_t kFontGlyphCount{size_name};\n")
            out.write(f"constexpr uint8_t kFontLineHeight{size_name} = {FONT_SIZES[size_name]};\n\n")
        out.write("// Every Vietnamese vowel x every tone (both cases) + the Vietnamese-only\n")
        out.write("// consonants -- for the 'fonttest' UART console command to dump against the\n")
        out.write("// real font table on real hardware.\n")
        out.write("extern const char kVietnameseTestString[];\n")

    with source.open("w") as out:
        out.write("// GENERATED by scripts/gen_font.py -- do not hand-edit.\n")
        out.write("#include \"font_noto_sans_vi.h\"\n\n")
        for size_name, (glyphs, _missing) in sizes_glyphs.items():
            emit_glyph_table_cc(out, size_name, glyphs)
        escaped = test_string.replace("\\", "\\\\").replace('"', '\\"')
        out.write(f'const char kVietnameseTestString[] = "{escaped}";\n')

    print(f"Wrote {header} and {source}")


def generate_qr_bitmap(url, target_pixels=200):
    import qrcode

    qr = qrcode.QRCode(error_correction=qrcode.constants.ERROR_CORRECT_M, box_size=1, border=4)
    qr.add_data(url)
    qr.make(fit=True)
    matrix = qr.get_matrix()  # list of lists of bool, includes the border
    modules = len(matrix)
    scale = max(1, target_pixels // modules)
    size_px = modules * scale

    row_bytes = (size_px + 7) // 8
    bitmap = bytearray(row_bytes * size_px)
    for my in range(modules):
        for mx in range(modules):
            if not matrix[my][mx]:
                continue
            for dy in range(scale):
                y = my * scale + dy
                for dx in range(scale):
                    x = mx * scale + dx
                    bitmap[y * row_bytes + x // 8] |= 0x80 >> (x % 8)

    return bytes(bitmap), size_px, size_px


def write_qr_files(url):
    bitmap, width, height = generate_qr_bitmap(url)
    header = OUTPUT_DIR / "qr_download.h"
    source = OUTPUT_DIR / "qr_download.cc"

    with header.open("w") as out:
        out.write("// GENERATED by scripts/gen_font.py -- do not hand-edit.\n")
        out.write(f"// QR code for {url}\n")
        out.write("#pragma once\n\n#include <cstdint>\n#include <cstddef>\n\n")
        out.write("extern const uint8_t kQrDownloadBitmap[];\n")
        out.write(f"constexpr int kQrDownloadWidth = {width};\n")
        out.write(f"constexpr int kQrDownloadHeight = {height};\n")

    with source.open("w") as out:
        out.write("// GENERATED by scripts/gen_font.py -- do not hand-edit.\n")
        out.write("#include \"qr_download.h\"\n\n")
        bytes_str = ", ".join(f"0x{b:02X}" for b in bitmap)
        out.write(f"const uint8_t kQrDownloadBitmap[] = {{{bytes_str}}};\n")

    print(f"Wrote {header} and {source} ({width}x{height}, {len(bitmap)} bytes)")


def render_preview(sizes_glyphs, qr_bitmap_wh, out_path):
    """Renders both planned screens to a PNG at the real 240x400 resolution,
    using Pillow directly (not the generated tables) -- this is the only
    pre-hardware visual check available."""
    qr_bitmap, qr_w, qr_h = qr_bitmap_wh
    canvas = Image.new("RGB", (240, 800), "black")  # two 240x400 screens stacked
    draw = ImageDraw.Draw(canvas)
    font24 = ImageFont.truetype(FONT_PATH, 24)
    font18 = ImageFont.truetype(FONT_PATH, 18)

    # --- Pairing screen (top 400px) ---
    def wrapped(text, font, max_width):
        words = text.split(" ")
        lines, current = [], ""
        for w in words:
            trial = (current + " " + w).strip()
            if font.getlength(trial) <= max_width:
                current = trial
            else:
                lines.append(current)
                current = w
        if current:
            lines.append(current)
        return lines

    y = 20
    for line in wrapped(PAIRING_TITLE, font24, 220):
        w = font24.getlength(line)
        draw.text(((240 - w) / 2, y), line, font=font24, fill="white")
        y += 30
    y += 10
    for line in wrapped(PAIRING_HINT, font18, 220):
        w = font18.getlength(line)
        draw.text(((240 - w) / 2, y), line, font=font18, fill="white")
        y += 24

    # QR code, rendered from the actual generated bitmap (not re-rendered
    # fresh) so the preview also validates the bitmap packing round-trips.
    qr_img = Image.new("1", (qr_w, qr_h), 0)
    row_bytes = (qr_w + 7) // 8
    for py in range(qr_h):
        for px in range(qr_w):
            bit = (qr_bitmap[py * row_bytes + px // 8] >> (7 - (px % 8))) & 1
            if bit:
                qr_img.putpixel((px, py), 1)
    qr_rgb = Image.new("RGB", (qr_w, qr_h), "black")
    qr_rgb.paste(Image.new("RGB", (qr_w, qr_h), "white"), mask=qr_img)
    canvas.paste(qr_rgb, ((240 - qr_w) // 2, y + 10))

    # --- Dashboard screen (bottom 400px, offset by 400) ---
    sample_values = ["68%", "24.5°C", "55%", "42%"]
    y = 460
    for label, value in zip(DASHBOARD_LINES, sample_values):
        text = label.format(value)
        draw.text((20, y), text, font=font18, fill="white")
        y += 40

    canvas.save(out_path)
    print(f"Wrote preview to {out_path}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--preview", metavar="PNG_PATH", nargs="?", const="/tmp/synora_font_preview.png",
                         help="also write a PNG mockup of both screens to this path "
                              "(default /tmp/synora_font_preview.png -- a scratch/dev-tool artifact, "
                              "deliberately not written under the repo)")
    args = parser.parse_args()

    sizes_glyphs = {}
    for size_name, size_px in FONT_SIZES.items():
        glyphs, missing = rasterize_font(size_px)
        sizes_glyphs[size_name] = (glyphs, missing)
        if missing:
            print(f"WARNING: {len(missing)} codepoints had no glyph at size {size_name}: "
                  f"{[hex(c) for c in missing[:10]]}{'...' if len(missing) > 10 else ''}", file=sys.stderr)
        else:
            print(f"Size {size_name}: {len(glyphs)} glyphs rasterized, no coverage gaps")

    write_font_files(sizes_glyphs)
    write_qr_files(DOWNLOAD_URL)

    if args.preview:
        bitmap, w, h = generate_qr_bitmap(DOWNLOAD_URL)
        render_preview(sizes_glyphs, (bitmap, w, h), args.preview)


if __name__ == "__main__":
    main()
