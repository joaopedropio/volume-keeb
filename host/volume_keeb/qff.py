"""TrueType/OpenType font -> Quantum Painter QFF file, for one of the
knob's font slots.

A from-scratch take on QMK's `qmk painter-make-font-image` +
`painter-convert-font-image` (lib/python/qmk/painter_qff.py and
painter.py), so the host doesn't need the QMK CLI. The format is
https://docs.qmk.fm/quantum_painter_qff: printable ASCII only, one
grayscale bitmap per glyph, RLE'd when that's smaller.

Unlike QMK's tool, the size isn't picked by the user: the knob's layout is
built around each slot's line height, so the font is rendered at the
biggest size whose glyphs all fit in it (and in FONT_MAX_GLYPH_W).

Run as `python3 -m volume_keeb.qff FONT SLOT OUT.qff [FORMAT]` to convert
one by hand (e.g. for tools/ui_sim's UI_SIM_FONTS).
"""
import io
import struct
import sys

from PIL import Image, ImageDraw, ImageFont

from .protocol import FONT_MAX_BYTES, FONT_MAX_GLYPH_W, FONT_SLOTS

GLYPHS = [chr(c) for c in range(0x20, 0x7F)]

# Grayscale formats: bits per pixel, and the image format byte (QMK's
# qp_internal_formats.h).
FORMATS = {
    "mono2": {"bpp": 1, "id": 0x00, "label": "1-bit (sharp, for pixel fonts)"},
    "mono4": {"bpp": 2, "id": 0x01, "label": "2-bit (light smoothing)"},
    "mono16": {"bpp": 4, "id": 0x02, "label": "4-bit (smooth)"},
}


class FontError(ValueError):
    pass


def _fits(font, height):
    top = bottom = 0
    for ch in GLYPHS:
        _, t, r, b = font.getbbox(ch, anchor="ls")
        top, bottom = min(top, t), max(bottom, b)
        if max(r, round(font.getlength(ch))) > FONT_MAX_GLYPH_W:
            return None
    return (top, bottom) if bottom - top <= height else None


def render(font_bytes, height, fmt):
    """One `height`-tall grayscale image per glyph ('L' mode, 0 = background),
    plus the point size picked."""
    try:
        ImageFont.truetype(io.BytesIO(font_bytes), height)
    except OSError as e:
        raise FontError(f"not a font Pillow can read ({e})") from None
    for size in range(height * 2, 3, -1):
        font = ImageFont.truetype(io.BytesIO(font_bytes), size)
        extent = _fits(font, height)
        if extent:
            break
    else:
        raise FontError("no size of this font fits the slot")
    top, bottom = extent
    baseline = -top + (height - (bottom - top)) // 2

    images = []
    for ch in GLYPHS:
        left, _, right, _ = font.getbbox(ch, anchor="ls")
        shift = max(0, -left)  # ink left of the pen position
        width = min(max(right, round(font.getlength(ch))) + shift, FONT_MAX_GLYPH_W)
        img = Image.new("L", (max(width, 1), height), 0)
        draw = ImageDraw.Draw(img)
        if FORMATS[fmt]["bpp"] == 1:
            draw.fontmode = "1"  # thresholding anti-aliased edges looks worse than no AA
        draw.text((shift, baseline), ch, font=font, fill=255, anchor="ls")
        images.append(img)
    return images, size


def _quantize(img, bpp):
    top = (1 << bpp) - 1
    return [round(v * top / 255) for v in img.getdata()]


def _pack(levels, bpp):
    per_byte = 8 // bpp
    out = bytearray()
    for i in range(0, len(levels), per_byte):
        byte = 0
        for n, level in enumerate(levels[i : i + per_byte]):
            byte |= level << (n * bpp)
        out.append(byte)
    return bytes(out)


def _rle(data):
    """QMK's RLE: a byte < 128 is "repeat the next byte that many times", a
    byte >= 128 is "copy the next (byte - 127) bytes"."""
    out = bytearray()
    i, n = 0, len(data)
    literal = bytearray()

    def flush_literal():
        for j in range(0, len(literal), 128):
            part = literal[j : j + 128]
            out.append(127 + len(part))
            out.extend(part)
        literal.clear()

    while i < n:
        run = 1
        while i + run < n and data[i + run] == data[i] and run < 127:
            run += 1
        if run >= 2:
            flush_literal()
            out.append(run)
            out.append(data[i])
            i += run
        else:
            literal.append(data[i])
            i += 1
    flush_literal()
    return bytes(out)


def _block(type_id, length):
    return bytes([type_id, ~type_id & 0xFF]) + length.to_bytes(3, "little")


def encode(images, fmt):
    """The QFF file for these glyph images."""
    bpp = FORMATS[fmt]["bpp"]
    height = images[0].height
    raw = [_pack(_quantize(img, bpp), bpp) for img in images]
    rle = [_rle(r) for r in raw]
    use_rle = sum(map(len, rle)) < sum(map(len, raw))
    glyph_data = rle if use_rle else raw

    table = bytearray()
    data = bytearray()
    for img, blob in zip(images, glyph_data):
        table += ((len(data) << 6) | img.width).to_bytes(3, "little")
        data += blob
    if len(data) >= 1 << 18:
        raise FontError("font too big for QFF's glyph offsets")

    ascii_block = _block(0x01, len(table)) + table
    data_block = _block(0x04, len(data)) + data
    total = 5 + 20 + len(ascii_block) + len(data_block)
    descriptor = _block(0x00, 20) + (0x464651).to_bytes(3, "little") + bytes([1])
    descriptor += struct.pack("<II", total, ~total & 0xFFFFFFFF)
    descriptor += bytes([height, 1]) + struct.pack("<H", 0)  # line height, ASCII table, no unicode glyphs
    descriptor += bytes([FORMATS[fmt]["id"], 0, 1 if use_rle else 0, 0xFF])  # format, flags, compression, transparency
    return descriptor + ascii_block + data_block


def convert(font_bytes, slot, fmt):
    """(QFF bytes, point size) for `slot` (a FONT_SLOTS key)."""
    if fmt not in FORMATS:
        raise FontError(f"unknown format {fmt}")
    images, size = render(font_bytes, FONT_SLOTS[slot]["height"], fmt)
    qff = encode(images, fmt)
    if len(qff) > FONT_MAX_BYTES:
        raise FontError(f"converted font is {len(qff) // 1024} kB, a slot holds {FONT_MAX_BYTES // 1024} kB; try a lower bit depth")
    return qff, size


def preview(font_bytes, slot, fmt, text, fg=(255, 255, 255), bg=(0, 0, 0), scale=2):
    """PNG of `text` drawn the way the knob will draw it."""
    images, _ = render(font_bytes, FONT_SLOTS[slot]["height"], fmt)
    bpp = FORMATS[fmt]["bpp"]
    top = (1 << bpp) - 1
    glyphs = [images[ord(c) - 0x20] for c in text if 0x20 <= ord(c) < 0x7F]
    width = max(1, sum(g.width for g in glyphs))
    out = Image.new("RGB", (width, images[0].height), bg)
    x = 0
    for g in glyphs:
        levels = _quantize(g, bpp)
        mask = Image.new("L", g.size)
        mask.putdata([round(v * 255 / top) for v in levels])
        out.paste(Image.new("RGB", g.size, fg), (x, 0), mask)
        x += g.width
    out = out.resize((out.width * scale, out.height * scale), Image.NEAREST)
    buf = io.BytesIO()
    out.save(buf, "PNG")
    return buf.getvalue()


if __name__ == "__main__":
    if len(sys.argv) not in (4, 5):
        sys.exit(__doc__)
    font_path, slot, out = sys.argv[1:4]
    fmt = sys.argv[4] if len(sys.argv) == 5 else "mono16"
    with open(font_path, "rb") as f:
        qff, size = convert(f.read(), slot, fmt)
    with open(out, "wb") as f:
        f.write(qff)
    print(f"{out}: {len(qff)} bytes, {size} pt")
