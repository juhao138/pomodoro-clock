# -*- coding: utf-8 -*-
"""生成番茄钟的图标：深色圆角底 + 番茄红进度环（和界面同风格）。

输出 _build/clock.ico（多尺寸）。
"""
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "_build" / "clock.ico"

BG = (18, 24, 38, 255)          # #121826
RING = (255, 107, 90, 255)      # #FF6B5A
TRACK = (35, 43, 61, 255)       # #232B3D
DOT = (255, 255, 255, 255)


def draw(size: int) -> Image.Image:
    ss = 4                                   # 超采样，缩小后边缘更干净
    n = size * ss
    img = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    # 圆角底
    pad = n * 0.055
    d.rounded_rectangle([pad, pad, n - pad, n - pad],
                        radius=n * 0.235, fill=BG)

    # 环
    thick = n * 0.115
    inset = n * 0.20
    box = [inset, inset, n - inset, n - inset]
    d.arc(box, start=0, end=360, fill=TRACK, width=int(thick))

    # 进度弧：从 12 点顺时针 300°，留 60° 缺口
    d.arc(box, start=-90, end=-90 + 300, fill=RING, width=int(thick))

    # 弧末端圆头
    import math
    ang = math.radians(-90 + 300)
    r = (box[2] - box[0]) / 2
    cx = (box[0] + box[2]) / 2
    cy = (box[1] + box[3]) / 2
    ex, ey = cx + r * math.cos(ang), cy + r * math.sin(ang)
    rr = thick / 2
    d.ellipse([ex - rr, ey - rr, ex + rr, ey + rr], fill=RING)
    # 起始圆头
    ang0 = math.radians(-90)
    sx, sy = cx + r * math.cos(ang0), cy + r * math.sin(ang0)
    d.ellipse([sx - rr, sy - rr, sx + rr, sy + rr], fill=RING)

    # 中心点（像秒针轴）
    cr = n * 0.022
    d.ellipse([n / 2 - cr, n / 2 - cr, n / 2 + cr, n / 2 + cr], fill=DOT)

    return img.resize((size, size), Image.LANCZOS)


def main() -> None:
    sizes = [256, 128, 64, 48, 32, 24, 16]
    frames = [draw(s) for s in sizes]
    frames[0].save(OUT, format="ICO", sizes=[(s, s) for s in sizes],
                   append_images=frames[1:])
    print(f"{OUT.name} 生成完成，{len(sizes)} 个尺寸")


if __name__ == "__main__":
    main()
