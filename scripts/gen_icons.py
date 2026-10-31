#!/usr/bin/env python3
"""Generate 1-bit icons as LVGL image C arrays for the e-paper display."""
from __future__ import annotations

from pathlib import Path
from PIL import Image, ImageDraw

ASSETS = Path(__file__).resolve().parent.parent / "main" / "assets"
ASSETS.mkdir(parents=True, exist_ok=True)


def pack_bits(pixels: list[list[int]]) -> bytes:
    """Pack a 2D 0/1 grid into MSB-first row bytes."""
    h = len(pixels)
    w = len(pixels[0]) if h else 0
    wb = (w + 7) // 8
    out = bytearray()
    for y in range(h):
        row = pixels[y]
        for xb in range(wb):
            byte = 0
            for i in range(8):
                x = xb * 8 + i
                if x < w and row[x]:
                    byte |= 0x80 >> i
            out.append(byte)
    return bytes(out)


def make_droplet(w: int = 14, h: int = 18) -> bytes:
    """Teardrop: a circle at the bottom with a point on top."""
    img = Image.new("1", (w, h), 0)
    draw = ImageDraw.Draw(img)
    r = w / 2
    cy = h - r
    draw.ellipse([0, h - w, w - 1, h - 1], fill=1)
    draw.polygon([(r - 0.5, 0), (w - 1, cy), (0, cy)], fill=1)
    return pack_bits([[img.getpixel((x, y)) for x in range(w)] for y in range(h)])


def write_c_array(name: str, width: int, height: int, data: bytes, path: Path) -> None:
    body = ", ".join(f"0x{b:02x}" for b in data)
    path.write_text(
        f"#pragma once\n\n"
        f"/* LVGL 1-bit image: {name} ({width}x{height}) */\n"
        f"#define ICON_{name.upper()}_WIDTH {width}\n"
        f"#define ICON_{name.upper()}_HEIGHT {height}\n"
        f"static const uint8_t icon_{name}[] = {{\n    {body}\n}};\n\n"
    )


def main() -> None:
    icons = {"droplet": (make_droplet, 14, 18)}
    for name, (maker, w, h) in icons.items():
        write_c_array(name, w, h, maker(w, h), ASSETS / f"icon_{name}.h")

    icons_h = ASSETS / "icons.h"
    icons_h.write_text(
        "#pragma once\n\n"
        "#include <stdint.h>\n\n"
        + "\n".join(f'#include "icon_{name}.h"' for name in icons)
        + "\n"
    )

    print("Generated icons:", ", ".join(icons))


if __name__ == "__main__":
    main()
