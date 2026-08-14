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


def make_thermometer(size: int = 32) -> bytes:
    img = Image.new("1", (size, size), 0)
    draw = ImageDraw.Draw(img)
    cx, cy = size // 2, size // 2
    bulb_r = size // 4
    stem_w = max(4, size // 6)
    # stem
    draw.rounded_rectangle([cx - stem_w//2, cy - size//3, cx + stem_w//2, cy + size//3 - bulb_r//2], radius=stem_w//2, fill=1)
    # bulb
    draw.ellipse([cx - bulb_r, cy + size//3 - bulb_r, cx + bulb_r - 1, cy + size//3 + bulb_r - 1], fill=1)
    # highlight (white)
    draw.ellipse([cx - bulb_r//3, cy + size//3 - bulb_r//3, cx, cy + size//3], fill=0)
    return pack_bits([[img.getpixel((x, y)) for x in range(size)] for y in range(size)])


def make_droplet(size: int = 32) -> bytes:
    img = Image.new("1", (size, size), 0)
    draw = ImageDraw.Draw(img)
    cx, cy = size // 2, size // 2 + 1
    # droplet shape via filled polygon
    draw.polygon([(cx, 1), (size - 3, cy), (cx, size - 2), (3, cy)], fill=1)
    # small white highlight
    draw.ellipse([cx - size//6, cy - size//6, cx + 1, cy + 1], fill=0)
    return pack_bits([[img.getpixel((x, y)) for x in range(size)] for y in range(size)])


def make_battery(size: int = 32, level: int = 70) -> bytes:
    img = Image.new("1", (size, size), 0)
    draw = ImageDraw.Draw(img)
    w, h = size - 6, size - 8
    x, y = 3, 4
    # body
    draw.rectangle([x, y, x + w - 1, y + h - 1], outline=1, fill=0)
    # terminal
    draw.rectangle([x + w//3, y - 3, x + 2*w//3, y - 1], fill=1)
    fill_w = max(1, (w - 4) * level // 100)
    draw.rectangle([x + 2, y + 2, x + 2 + fill_w - 1, y + h - 3], fill=1)
    return pack_bits([[img.getpixel((x, y)) for x in range(size)] for y in range(size)])


def make_bluetooth(size: int = 32) -> bytes:
    img = Image.new("1", (size, size), 0)
    draw = ImageDraw.Draw(img)
    cx, cy = size // 2, size // 2
    # rune-like symbol
    draw.polygon([(cx, 1), (cx + size//3, cy - size//5), (cx, cy), (cx + size//3, cy + size//5)], fill=1)
    draw.polygon([(cx, size - 2), (cx - size//3, cy + size//5), (cx, cy), (cx - size//3, cy - size//5)], fill=1)
    draw.line([(cx, 1), (cx, size - 2)], fill=1, width=2)
    return pack_bits([[img.getpixel((x, y)) for x in range(size)] for y in range(size)])


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
    sizes = {"thermometer": 32, "droplet": 32, "battery": 32, "bluetooth": 32}
    makers = {
        "thermometer": make_thermometer,
        "droplet": make_droplet,
        "battery": make_battery,
        "bluetooth": make_bluetooth,
    }
    for name, maker in makers.items():
        size = sizes[name]
        data = maker(size)
        write_c_array(name, size, size, data, ASSETS / f"icon_{name}.h")

    # also create a consolidated icons.c so it can be compiled as a single file
    consolidated = ASSETS / "icons.c"
    parts = ['#include "icons.h"\n']
    for name in makers:
        parts.append(f'#include "icon_{name}.h"')
    consolidated.write_text("\n".join(parts) + "\n")

    icons_h = ASSETS / "icons.h"
    icons_h.write_text(
        "#pragma once\n\n"
        "#include <stdint.h>\n\n"
        + "\n".join(f'#include "icon_{name}.h"' for name in makers)
        + "\n"
    )

    print("Generated icons:", ", ".join(makers))


if __name__ == "__main__":
    main()
