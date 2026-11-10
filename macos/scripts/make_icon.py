#!/usr/bin/env python3
"""生成 macOS 应用图标：墨色方底 + 12 格刻度环 + 一格黄。

    python3 scripts/make_icon.py        # 输出 Resources/AppIcon.icns

需要 Pillow 和 iconutil（macOS 自带）。
"""

import math
import os
import shutil
import subprocess
import tempfile
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
RES = os.path.join(HERE, "..", "Resources")

INK = (22, 22, 22, 255)
PAPER = (244, 244, 242, 255)
SIGNAL = (255, 250, 0, 255)

S = 1024


def squircle_mask(size: int, radius: float) -> Image.Image:
    m = Image.new("L", (size * 2, size * 2), 0)
    d = ImageDraw.Draw(m)
    d.rounded_rectangle((0, 0, size * 2 - 1, size * 2 - 1), radius=radius * 2, fill=255)
    return m.resize((size, size), Image.LANCZOS)


def draw_icon(size: int) -> Image.Image:
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    # macOS 图标网格：主体占 82%
    inset = size * 0.09
    body = int(size - inset * 2)
    mask = squircle_mask(body, body * 0.225)

    layer = Image.new("RGBA", (body, body), INK)
    ld = ImageDraw.Draw(layer)

    cx = cy = body / 2
    ring = body * 0.255
    tick_len = body * 0.165
    tick_w = body * 0.052

    for i in range(12):
        ang = math.radians(-90 + i * 30)
        color = SIGNAL if i == 0 else PAPER
        ln = tick_len * (1.12 if i == 0 else 1.0)
        w = tick_w * (1.0 if i == 0 else 0.86)
        ux, uy = math.cos(ang), math.sin(ang)
        nx, ny = -uy, ux
        r0 = ring
        r1 = ring + ln
        pts = [
            (cx + ux * r0 + nx * w / 2, cy + uy * r0 + ny * w / 2),
            (cx + ux * r1 + nx * w / 2, cy + uy * r1 + ny * w / 2),
            (cx + ux * r1 - nx * w / 2, cy + uy * r1 - ny * w / 2),
            (cx + ux * r0 - nx * w / 2, cy + uy * r0 - ny * w / 2),
        ]
        ld.polygon(pts, fill=color)

    img.paste(layer, (int(inset), int(inset)), mask)
    return img


def main():
    src = os.path.join(HERE, "..", "Resources", "AppIcon.icns")
    iconset = tempfile.mkdtemp(suffix=".iconset")
    try:
        for px in (16, 32, 64, 128, 256, 512, 1024):
            icon = draw_icon(px)
            icon.save(os.path.join(iconset, f"icon_{px}x{px}.png"))
            if px <= 512:
                icon.save(os.path.join(iconset, f"icon_{px}x{px}@2x.png"))
        subprocess.run(["iconutil", "-c", "icns", iconset, "-o", src], check=True)
        print("已生成", os.path.normpath(src))
    finally:
        shutil.rmtree(iconset, ignore_errors=True)


if __name__ == "__main__":
    main()
