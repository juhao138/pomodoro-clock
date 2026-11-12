# -*- coding: utf-8 -*-
"""生成番茄钟图标：墨色圆角底 + 12 格刻度环 + 一格黄（和界面同一套语言）。

输出 _build/clock.ico（多尺寸）。
"""
import math
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "_build" / "clock.ico"

INK = (22, 22, 22, 255)        # #161616
PAPER = (244, 244, 242, 255)   # #F4F4F2
SIGNAL = (255, 250, 0, 255)    # #FFFA00


def draw(size: int) -> Image.Image:
    ss = 4
    n = size * ss
    img = Image.new("RGBA", (n, n), (0, 0, 0, 0))

    # 圆角底（Windows 用直角也常见，这里跟 macOS 版一致用圆角）
    base = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    bd = ImageDraw.Draw(base)
    bd.rounded_rectangle((0, 0, n - 1, n - 1), radius=int(n * 0.18), fill=INK)

    d = ImageDraw.Draw(base)
    cx = cy = n / 2.0
    ring = n * 0.255
    tick_len = n * 0.165
    tick_w = n * 0.052

    for i in range(12):
        ang = math.radians(-90 + i * 30)
        color = SIGNAL if i == 0 else PAPER
        ln = tick_len * (1.12 if i == 0 else 1.0)
        w = tick_w * (1.0 if i == 0 else 0.86)
        ux, uy = math.cos(ang), math.sin(ang)
        nx, ny = -uy, ux
        r0, r1 = ring, ring + ln
        pts = [
            (cx + ux * r0 + nx * w / 2, cy + uy * r0 + ny * w / 2),
            (cx + ux * r1 + nx * w / 2, cy + uy * r1 + ny * w / 2),
            (cx + ux * r1 - nx * w / 2, cy + uy * r1 - ny * w / 2),
            (cx + ux * r0 - nx * w / 2, cy + uy * r0 - ny * w / 2),
        ]
        d.polygon(pts, fill=color)

    img = base.resize((size, size), Image.LANCZOS)
    return img


def main():
    sizes = [16, 24, 32, 48, 64, 128, 256]
    frames = [draw(s) for s in sizes]
    frames[-1].save(OUT, format="ICO", sizes=[(s, s) for s in sizes])
    print("已生成", OUT)


if __name__ == "__main__":
    main()
