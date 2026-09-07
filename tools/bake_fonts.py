#!/usr/bin/env python3
"""Bake TTF/variable fonts into compact 1bpp glyph tables for BEACON.

Emits a single C header + source pair consumed by both the firmware and the
host simulator, plus a PNG proof sheet per face so rasterisation quality can
be reviewed without flashing anything.

Glyph bitmaps are row-packed MSB-first, stride = ceil(w/8), 1 = ink.
"""

from __future__ import annotations

import argparse
import dataclasses
import pathlib

from PIL import Image, ImageDraw, ImageFont

FIRST, LAST = 0x20, 0x7E
# Latin-1 / punctuation extras worth carrying for titles and paths.
EXTRA = "·•→←…–—°±×"

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent


@dataclasses.dataclass
class Face:
    name: str          # C identifier suffix
    file: str
    size: int
    axes: list[int] | None = None   # variable-font axis values
    threshold: int = 128            # 1bpp cut point on the antialiased render
    tracking: int = 0               # extra px added to every advance


FACES = [
    # -- Inter: the content voice ------------------------------------------
    # Small sizes are baked slightly heavier and at a lower 1bpp cut than the
    # TrueType outline suggests. On this 119 ppi panel a light cut loses the
    # joins in a, e, s and makes 10 px metadata read as noise; a heavier cut
    # costs a little elegance and buys the stroke the front layer eats.
    Face("ui10",   "Inter[opsz,wght].ttf", 10, [14, 620], threshold=112, tracking=1),
    Face("ui12",   "Inter[opsz,wght].ttf", 12, [14, 520], threshold=118),
    Face("ui12b",  "Inter[opsz,wght].ttf", 12, [14, 740], threshold=118),
    Face("ui14",   "Inter[opsz,wght].ttf", 14, [14, 450], threshold=120),
    Face("ui14b",  "Inter[opsz,wght].ttf", 14, [14, 740], threshold=122),
    Face("ui18b",  "Inter[opsz,wght].ttf", 18, [18, 720], threshold=124),
    Face("ui26b",  "Inter[opsz,wght].ttf", 26, [28, 800], threshold=126),
    Face("ui40b",  "Inter[opsz,wght].ttf", 40, [32, 800], threshold=124),
    # -- IBM Plex Mono: the instrument voice -------------------------------
    # 9 px Plex loses the vertex of M entirely; 11 px is where the caps still
    # hold their shape through the panel's front layer at a glance.
    Face("label",  "IBMPlexMono-SemiBold.ttf", 11, threshold=122, tracking=1),
    Face("mono11", "IBMPlexMono-Regular.ttf",  12, threshold=118),
    Face("mono11b","IBMPlexMono-SemiBold.ttf", 12, threshold=118),
]


def charset() -> list[str]:
    return [chr(c) for c in range(FIRST, LAST + 1)] + list(EXTRA)


def load(face: Face) -> ImageFont.FreeTypeFont:
    font = ImageFont.truetype(str(HERE / "fonts" / face.file), face.size)
    if face.axes:
        font.set_variation_by_axes([float(v) for v in face.axes])
    return font


def render_glyph(font: ImageFont.FreeTypeFont, ch: str, face: Face):
    """Return (w, h, left, top, rows[]) with top measured up from baseline."""
    pad = face.size * 2 + 8
    img = Image.new("L", (pad * 2, pad * 2), 0)
    draw = ImageDraw.Draw(img)
    origin = (pad, pad)
    draw.text(origin, ch, font=font, fill=255, anchor="ls")

    bw = img.point(lambda v: 255 if v >= face.threshold else 0, mode="1")
    box = bw.getbbox()
    advance = round(font.getlength(ch)) + face.tracking
    if box is None:
        return 0, 0, 0, 0, b"", advance

    x0, y0, x1, y1 = box
    w, h = x1 - x0, y1 - y0
    left = x0 - origin[0]
    top = origin[1] - y0          # positive = above baseline
    crop = bw.crop(box)
    px = crop.load()
    stride = (w + 7) // 8
    rows = bytearray(stride * h)
    for y in range(h):
        for x in range(w):
            if px[x, y]:
                rows[y * stride + (x >> 3)] |= 0x80 >> (x & 7)
    return w, h, left, top, bytes(rows), advance


def bake(face: Face):
    font = load(face)
    ascent, descent = font.getmetrics()
    glyphs, blob = [], bytearray()
    for ch in charset():
        w, h, left, top, rows, advance = render_glyph(font, ch, face)
        glyphs.append(
            dict(cp=ord(ch), w=w, h=h, left=left, top=top,
                 adv=advance, off=len(blob))
        )
        blob += rows
    return dict(
        face=face, glyphs=glyphs, blob=bytes(blob),
        ascent=ascent, descent=descent,
        line=ascent + descent,
    )


def proof(baked, out: pathlib.Path):
    """Render a proof sheet at 1:1 and 4x so small sizes can be judged."""
    face = baked["face"]
    lines = [
        "Hamburgefonstiv 0123456789",
        "BEACON / agent mission control",
        "blocked · working · idle → done",
        "~/dev/zectrix-note4  main  12m34s",
    ]
    font = load(face)
    width, lh = 460, baked["line"] + 3
    img = Image.new("1", (width, lh * len(lines) + 8), 1)
    d = ImageDraw.Draw(img)
    for i, text in enumerate(lines):
        d.text((6, 4 + i * lh + baked["ascent"]), text, font=font,
               fill=0, anchor="ls")
    big = img.resize((width * 3, img.height * 3), Image.NEAREST)
    sheet = Image.new("1", (width * 3, img.height + big.height + 10), 1)
    sheet.paste(img, (0, 2))
    sheet.paste(big, (0, img.height + 8))
    sheet.save(out)


def emit(all_baked, header: pathlib.Path, source: pathlib.Path):
    h = ["// Generated by tools/bake_fonts.py. Do not edit.",
         "#ifndef BEACON_FONTS_H_", "#define BEACON_FONTS_H_", "",
         '#include "beacon_font.h"', "",
         "#ifdef __cplusplus", 'extern "C" {', "#endif", ""]
    c = ['// Generated by tools/bake_fonts.py. Do not edit.',
         '#include "beacon_fonts.h"', ""]

    for b in all_baked:
        name = b["face"].name
        h.append(f"extern const beacon_font_t beacon_font_{name};")

        c.append(f"static const uint8_t k_{name}_bits[] = {{")
        blob = b["blob"]
        for i in range(0, len(blob), 20):
            c.append("    " + ", ".join(f"0x{v:02x}" for v in blob[i:i + 20]) + ",")
        c.append("};")
        c.append(f"static const beacon_glyph_t k_{name}_glyphs[] = {{")
        for g in b["glyphs"]:
            c.append(
                "    {{{off}, {adv}, {left}, {top}, {w}, {h}}},  // U+{cp:04X}"
                .format(**g)
            )
        c.append("};")
        c.append(f"static const uint16_t k_{name}_cps[] = {{")
        cps = [g["cp"] for g in b["glyphs"]]
        for i in range(0, len(cps), 12):
            c.append("    " + ", ".join(str(v) for v in cps[i:i + 12]) + ",")
        c.append("};")
        c.append(f"const beacon_font_t beacon_font_{name} = {{")
        c.append(f'    .name = "{name}",')
        c.append(f"    .ascent = {b['ascent']},")
        c.append(f"    .descent = {b['descent']},")
        c.append(f"    .line_height = {b['line']},")
        c.append(f"    .count = {len(b['glyphs'])},")
        c.append(f"    .codepoints = k_{name}_cps,")
        c.append(f"    .glyphs = k_{name}_glyphs,")
        c.append(f"    .bits = k_{name}_bits,")
        c.append("};")
        c.append("")

    h += ["", "#ifdef __cplusplus", "}", "#endif", "",
          "#endif  // BEACON_FONTS_H_"]
    header.write_text("\n".join(h) + "\n")
    source.write_text("\n".join(c) + "\n")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--outdir", default="firmware/components/beacon_gfx/generated")
    ap.add_argument("--proofs", default="sim/out/fonts")
    args = ap.parse_args()

    outdir = ROOT / args.outdir
    outdir.mkdir(parents=True, exist_ok=True)
    proofs = ROOT / args.proofs
    proofs.mkdir(parents=True, exist_ok=True)

    baked = []
    total = 0
    for face in FACES:
        b = bake(face)
        baked.append(b)
        total += len(b["blob"]) + len(b["glyphs"]) * 8
        proof(b, proofs / f"{face.name}.png")
        print(f"  {face.name:8s} {face.size:3d}px  "
              f"line={b['line']:2d}  bitmap={len(b['blob']):5d} B")

    emit(baked, outdir / "beacon_fonts.h", outdir / "beacon_fonts.c")
    print(f"total font ROM ~= {total/1024:.1f} KiB -> {outdir}")


if __name__ == "__main__":
    main()
