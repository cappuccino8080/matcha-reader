#!/usr/bin/env python3
"""Generate Hojicha Reader mascot bitmaps from the locally supplied artwork.

Source: assets/images/raccoon-dog-reading.png
This mascot is distinct from the original Matcha Reader cat artwork. Keep the
original project's licenses and the original cat artwork's license unchanged.

    python scripts/generate_mascot_assets.py [--preview] [--stamp-fonts DIR]

Writes the original, firmware-compatible output names:
  src/images/CatLogo.h
  src/components/icons/endOfBookCat.h
  src/network/html/assets/matcha-icon.svg
  src/components/icons/eobStamp.h       (only with --stamp-fonts)

The CatLogo.h name and CAT_LOGO_* symbols are intentionally retained for now.
The image is stored as 4-level RLE with a footprint bit; the mascot itself is
rendered in hard black/white to avoid dotted edges on the small E-Ink panel.
"""

import argparse
import base64
import io
import random
import zlib
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "assets" / "images" / "raccoon-dog-reading.png"

LOGO_WIDTH = 230
SMALL_LOGO_WIDTH = 130
EOB_SIZE = 152
WEB_SIZE = 128

# Output-size thresholds. No dithering: crisp edges are preferred for text.
INK_THRESHOLD = 190
# Group nearby strokes/letters into a shared white backing on dark screens.
FOOTPRINT_JOIN_SIZE = 7  # Pillow Max/Min filters require an odd integer.
FOOTPRINT_MARGIN_SIZE = 7


def solid_edge(art, width, height):
    """Return a clean 1-bit L image (0=ink, 255=paper), with no dithering."""
    small = art.resize((width, height), Image.Resampling.LANCZOS)
    ink = small.point(lambda v: 255 if v < INK_THRESHOLD else 0, "L")
    # No post-dilation here: even a 1 px closing step can merge small letters.
    # The backing mask is expanded separately in build_footprint().
    return ink.point(lambda v: 0 if v else 255, "L")


def bw(art, width, height):
    """Create a black/white logo, preserving solid text and contour lines."""
    return solid_edge(art, width, height)


def _fill_holes(ink):
    """Fill regions enclosed by a nonzero (255) ink mask; return a 0/255 mask."""
    w, h = ink.size
    px = ink.load()
    outside = bytearray(w * h)
    stack = [(x, y) for x in range(w) for y in (0, h - 1)]
    stack += [(x, y) for y in range(h) for x in (0, w - 1)]
    while stack:
        x, y = stack.pop()
        if 0 <= x < w and 0 <= y < h and not outside[y * w + x] and px[x, y] == 0:
            outside[y * w + x] = 1
            stack.extend(((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)))
    return Image.frombytes("L", (w, h), bytes(0 if o else 255 for o in outside))


def build_footprint(art, width, height, binary=None):
    """Build the white backing that makes black artwork visible in dark sleep mode.

    Unlike the original plain _fill_holes approach, nearby strokes are joined
    before filling. For artwork clipped by the right edge (the tea table), the
    nearby dark line ends are joined along that edge to close its white surface.
    This is a heuristic for the current mascot; use a separately authored mask
    if a future image needs precise open-region control.
    """
    if binary is None:
        binary = bw(art, width, height)

    # Black in binary -> 255 in mask. Connect letters and fragmented contours.
    ink = binary.point(lambda v: 255 if v < 128 else 0, "L")
    ink = ink.filter(ImageFilter.MaxFilter(FOOTPRINT_JOIN_SIZE))
    ink = ink.filter(ImageFilter.MinFilter(FOOTPRINT_JOIN_SIZE))

    # The tea table extends off the image's right edge. Its top/bottom lines
    # otherwise have an opening, so flood fill treats the tabletop as outside.
    edge_band = min(8, width)
    right_hits = []
    px = ink.load()
    for y in range(height // 2, height):
        xs = [x for x in range(width - edge_band, width) if px[x, y] != 0]
        if xs:
            right_hits.append((y, max(xs)))

    if len(right_hits) >= 2:
        ymin = right_hits[0][0]
        ymax = right_hits[-1][0]
        # Avoid bridging the whole image if an unrelated illustration reaches
        # the border over an excessively large vertical range.
        if 2 <= ymax - ymin <= height // 3:
            draw = ImageDraw.Draw(ink)
            for y, x in right_hits:
                draw.line((x, y, width - 1, y), fill=255)
            draw.line((width - 1, ymin, width - 1, ymax), fill=255)

    footprint = _fill_holes(ink)
    # Add paper around dark letterforms and the mascot, so black lines do not
    # disappear into a black sleep-screen background.
    footprint = footprint.filter(ImageFilter.MaxFilter(FOOTPRINT_MARGIN_SIZE))
    return footprint


def build_logo(art, width=LOGO_WIDTH):
    """Return width, height, packed RLE for the existing GrayLogo decoder."""
    height = round(art.size[1] * width / art.size[0])
    ink = bw(art, width, height)
    footprint = build_footprint(art, width, height, ink)

    # Preserve the original 3-bit symbol scheme (2-bit level + footprint).
    # This artwork deliberately uses only pure black (0) and white (3).
    levels = (0 if pixel < 128 else 3 for pixel in ink.tobytes())
    backing = (pixel >= 128 for pixel in footprint.tobytes())
    symbols = [level | (int(inside) << 2) for level, inside in zip(levels, backing)]

    rle = bytearray()
    i = 0
    while i < len(symbols):
        run = 1
        while i + run < len(symbols) and symbols[i + run] == symbols[i] and run < 32:
            run += 1
        rle.append(symbols[i] << 5 | (run - 1))
        i += run
    return width, height, rle


def build_end_of_book(art):
    width = round(art.size[0] * EOB_SIZE / art.size[1])
    small = bw(art, width, EOB_SIZE)
    square = Image.new("L", (EOB_SIZE, EOB_SIZE), 255)
    square.paste(small, ((EOB_SIZE - width) // 2, 0))
    # drawIcon plots stored (row, col) at screen (size - 1 - row, col).
    row_bytes = (EOB_SIZE + 7) // 8
    packed = bytearray([0xFF] * (row_bytes * EOB_SIZE))
    for row in range(EOB_SIZE):
        for col in range(EOB_SIZE):
            if square.getpixel((EOB_SIZE - 1 - row, col)) < 128:
                packed[row * row_bytes + (col >> 3)] &= ~(0x80 >> (col & 7)) & 0xFF
    return packed


def build_web_icon(art):
    scale = WEB_SIZE / max(art.size)
    w = round(art.size[0] * scale)
    h = round(art.size[1] * scale)
    small = bw(art, w, h)
    alpha = build_footprint(art, w, h, small)
    rgba = Image.merge("RGBA", (small, small, small, alpha))
    icon = Image.new("RGBA", (WEB_SIZE, WEB_SIZE), (0, 0, 0, 0))
    icon.paste(rgba, ((WEB_SIZE - w) // 2, (WEB_SIZE - h) // 2))
    buf = io.BytesIO()
    icon.save(buf, "PNG", optimize=True)
    data = base64.b64encode(buf.getvalue()).decode()
    return (
        "<!-- Hojicha Reader raccoon-dog mascot. -->\n"
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{WEB_SIZE}" height="{WEB_SIZE}" '
        f'viewBox="0 0 {WEB_SIZE} {WEB_SIZE}"><image width="{WEB_SIZE}" height="{WEB_SIZE}" '
        f'href="data:image/png;base64,{data}"/></svg>\n'
    )


STAMP_SIZE = 220

# The Japanese and Chinese end-of-book stamps are otherwise unchanged.
STAMPS = (
    ("EobStampJa", "読了", "NotoSerifJP.ttf"),
    ("EobStampZhHans", "读完", "NotoSerifSC.ttf"),
    ("EobStampZhHant", "讀完", "NotoSerifTC.ttf"),
    ("EobStampYue", "睇完", "NotoSerifHK.ttf"),
)


def build_stamp(font_path, text):
    """Hanko RLE, same byte format as the original Matcha Reader generator."""
    from PIL import ImageDraw, ImageFont

    scale = 4
    size = STAMP_SIZE * scale
    img = Image.new("L", (size, size), 0)
    draw = ImageDraw.Draw(img)
    draw.ellipse([4 * scale, 4 * scale, size - 4 * scale, size - 4 * scale], outline=255, width=6 * scale)
    inner = 17 * scale
    draw.ellipse([inner, inner, size - inner, size - inner], outline=255, width=2 * scale)
    font = ImageFont.truetype(str(font_path), int(size * 0.30))
    font.set_variation_by_axes([700])
    left, top, right, bottom = draw.textbbox((0, 0), text, font=font)
    x = (size - (right - left)) / 2 - left
    y = size * 0.38 - (bottom - top) / 2 - top
    draw.text((x, y), text, fill=255, font=font)
    img = img.resize((STAMP_SIZE, STAMP_SIZE), Image.Resampling.LANCZOS).point(lambda v: 255 if v > 110 else 0)
    rng = random.Random(3)
    px = img.load()
    for _ in range(STAMP_SIZE * STAMP_SIZE // 18):
        px[rng.randrange(STAMP_SIZE), rng.randrange(STAMP_SIZE)] = 0
    bits = [1 if v else 0 for v in img.tobytes()]
    rle = bytearray()
    i = 0
    while i < len(bits):
        run = 1
        while i + run < len(bits) and bits[i + run] == bits[i] and run < 128:
            run += 1
        rle.append(bits[i] << 7 | (run - 1))
        i += run
    return rle


def deflate(data):
    compressor = zlib.compressobj(level=9, wbits=-15)
    return compressor.compress(bytes(data)) + compressor.flush()


def c_array(data, per_line=19):
    lines = []
    for i in range(0, len(data), per_line):
        lines.append("    " + ", ".join(f"0x{b:02X}" for b in data[i : i + per_line]) + ",")
    return "\n".join(lines)


def write_preview(art):
    """Save enlarged views of the regular generated logo for visual checks."""
    w = LOGO_WIDTH
    h = round(art.height * w / art.width)
    binary = bw(art, w, h)
    footprint = build_footprint(art, w, h, binary)
    # Dark screen: white artwork only within the footprint; black stays black.
    dark = Image.new("L", (w, h), 0)
    dark.paste(binary, mask=footprint)
    light = binary
    out_dir = ROOT / ".pio"
    out_dir.mkdir(parents=True, exist_ok=True)
    for name, img in (("mascot-preview-dark.png", dark), ("mascot-preview-light.png", light)):
        path = out_dir / name
        img.resize((w * 3, h * 3), Image.Resampling.NEAREST).save(path)
        print(f"Preview: {path}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--stamp-fonts", type=Path, help="folder with NotoSerif{JP,SC,TC,HK}.ttf")
    parser.add_argument("--preview", action="store_true", help="save dark/light logo previews into .pio/")
    args = parser.parse_args()

    if not SOURCE.is_file():
        parser.error(f"mascot source not found: {SOURCE}")

    if args.stamp_fonts:
        arrays = []
        for name, text, font in STAMPS:
            rle = build_stamp(args.stamp_fonts / font, text)
            arrays.append(f"// {text}, lettered in {font[:-4]}\nstatic const uint8_t {name}[] = {{\n{c_array(rle)}\n}};\n")
        (ROOT / "src" / "components" / "icons" / "eobStamp.h").write_text(
            "#pragma once\n#include <cstdint>\n\n"
            "// The end-of-book hanko, one per wording. Run-length coded, row-major and upright: each byte is\n"
            "// (ink << 7) | (run - 1), runs of 1-128 pixels continuing across rows. Lettered in Noto Serif\n"
            "// (SIL Open Font License); the date inside is drawn at runtime.\n"
            "// Generated by scripts/generate_mascot_assets.py --stamp-fonts.\n"
            f"static constexpr int EOB_STAMP_SIZE = {STAMP_SIZE};\n\n" + "\n".join(arrays)
        )

    # Preserve grayscale source loading. Transparent PNGs should have a white
    # background before conversion, because dark-mode backing is mask-based.
    art = Image.open(SOURCE).convert("L")
    width, height, logo = build_logo(art)
    small_width, small_height, small_logo = build_logo(art, SMALL_LOGO_WIDTH)

    (ROOT / "src" / "images" / "CatLogo.h").write_text(
        "#pragma once\n#include <cstdint>\n\n"
        f"// {width}x{height} Hojicha Reader mascot, RLE: (symbol << 5) | (run - 1), runs of 1-32.\n"
        "// symbol = (footprint << 2) | level; level 0=black, 3=white (compatible 4-level format).\n"
        "// Generated by scripts/generate_mascot_assets.py from assets/images/raccoon-dog-reading.png.\n"
        f"static constexpr int CAT_LOGO_WIDTH = {width};\n"
        f"static constexpr int CAT_LOGO_HEIGHT = {height};\n"
        f"static const uint8_t CatLogoRle[] = {{\n{c_array(logo)}\n}};\n\n"
        f"static constexpr int CAT_LOGO_SMALL_WIDTH = {small_width};\n"
        f"static constexpr int CAT_LOGO_SMALL_HEIGHT = {small_height};\n"
        f"static constexpr int CAT_LOGO_SMALL_RLE_SIZE = {len(small_logo)};\n"
        f"static const uint8_t CatLogoSmallDeflated[] = {{\n{c_array(deflate(small_logo))}\n}};\n"
    )
    (ROOT / "src" / "components" / "icons" / "endOfBookCat.h").write_text(
        "#pragma once\n#include <cstdint>\n\n"
        "// Hojicha Reader raccoon-dog mascot for the end-of-book screen, 1 bpp drawIcon format.\n"
        "// Generated by scripts/generate_mascot_assets.py from assets/images/raccoon-dog-reading.png.\n"
        f"static constexpr int END_OF_BOOK_CAT_SIZE = {EOB_SIZE};\n"
        f"static const uint8_t EndOfBookCat[] = {{\n{c_array(build_end_of_book(art))}\n}};\n"
    )
    (ROOT / "src" / "network" / "html" / "assets" / "matcha-icon.svg").write_text(build_web_icon(art))
    print(f"Generated mascot: {width}x{height}; small: {small_width}x{small_height}")

    if args.preview:
        write_preview(art)


if __name__ == "__main__":
    main()
