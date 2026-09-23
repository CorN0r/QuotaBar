#!/usr/bin/env python3
"""把 provider_raw 里的原厂图标归一化为 128px 圆角芯片 PNG（assets/provider/*.png）。
芯片样式 = 各家桌面应用官方图标的呈现方式，深浅底均可读。
"""
from pathlib import Path

from PIL import Image, ImageDraw

RAW = Path(__file__).parent
OUT = RAW.parent / "provider"
SIZE = 128
RADIUS = 26  # 圆角约占 20%


def rounded_chip(fill: tuple) -> Image.Image:
    img = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([0, 0, SIZE - 1, SIZE - 1], radius=RADIUS, fill=fill)
    return img


def paste_fit(base: Image.Image, icon: Image.Image, frac: float) -> Image.Image:
    inner = int(SIZE * frac)
    ic = icon.resize((inner, inner), Image.LANCZOS)
    off = (SIZE - inner) // 2
    base.paste(ic, (off, off), ic)
    return base


def round_existing(img: Image.Image, radius: int = RADIUS) -> Image.Image:
    img = img.resize((SIZE, SIZE), Image.LANCZOS)
    mask = Image.new("L", (SIZE, SIZE), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, SIZE - 1, SIZE - 1], radius=radius, fill=255)
    img.putalpha(mask)
    return img


def kimi() -> Image.Image:
    # 48px 官方 favicon（黑底白 K + 蓝点）放大铺满圆角芯片
    im = Image.open(RAW / "kimi-light.ico")
    im.size = (48, 48)
    fr = im.convert("RGBA").resize((SIZE, SIZE), Image.LANCZOS)
    return round_existing(fr)


def codex() -> Image.Image:
    # openai apple-touch：白底黑结环 → 反白抠出，透明底无芯片，按包围盒放大至 ~98% 填充
    src = Image.open(RAW / "openai-touch.png").convert("RGBA")
    knot = Image.new("RGBA", src.size, (0, 0, 0, 0))
    px, out = src.load(), knot.load()
    for y in range(src.height):
        for x in range(src.width):
            r, g, b, _ = px[x, y]
            lum = 0.299 * r + 0.587 * g + 0.114 * b
            if lum < 190:  # 白底(255) → 全透明；结环主体(≤110) → 全实
                a = 255 if lum <= 110 else int(255 * (190 - lum) / 80)
                out[x, y] = (255, 255, 255, a)
    bbox = knot.split()[3].point(lambda v: 255 if v > 30 else 0).getbbox()
    knot = knot.crop(bbox)
    side = max(knot.size)
    sq = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    sq.paste(knot, ((side - knot.width) // 2, (side - knot.height) // 2), knot)
    out_img = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    inner = int(SIZE * 0.98)
    ic = sq.resize((inner, inner), Image.LANCZOS)
    off = (SIZE - inner) // 2
    out_img.paste(ic, (off, off), ic)
    return out_img


def glm() -> Image.Image:
    # 智谱 bigmodel favicon：自带深色圆角芯片白 Z（四角透明），直接放大
    im = Image.open(RAW / "bigmodel.png").convert("RGBA")
    return round_existing(im, radius=24)


def deepseek() -> Image.Image:
    # 白色芯片 + 官方蓝鲸（源 ico 为透明底鲸鱼）
    im = Image.open(RAW / "deepseek.ico")
    im.size = (225, 225)
    whale = im.convert("RGBA")
    chip = rounded_chip((255, 255, 255, 255))
    return paste_fit(chip, whale, 0.78)


def main() -> None:
    OUT.mkdir(exist_ok=True)
    for name, fn in [("kimi", kimi), ("codex", codex), ("glm", glm), ("deepseek", deepseek)]:
        img = fn()
        assert img.size == (SIZE, SIZE)
        img.save(OUT / f"{name}.png")
        # 浅/深底预览拼图
        prev = Image.new("RGB", (SIZE * 2 + 18, SIZE + 16), (255, 255, 255))
        light = Image.new("RGB", (SIZE, SIZE), (246, 246, 248))
        light.paste(img, (0, 0), img)
        dark = Image.new("RGB", (SIZE, SIZE), (32, 32, 36))
        dark.paste(img, (0, 0), img)
        prev.paste(light, (6, 8))
        prev.paste(dark, (SIZE + 12, 8))
        prev.save(RAW / f"final_{name}.png")
    sheet = Image.new("RGB", (SIZE * 4 + 50, SIZE + 16), (32, 32, 36))
    x = 10
    for name in ["kimi", "codex", "glm", "deepseek"]:
        img = Image.open(OUT / f"{name}.png")
        sheet.paste(img, (x, 8), img)
        x += SIZE + 10
    sheet.save(RAW / "sheet_final_dark.png")
    print("done:", [p.name for p in OUT.glob('*.png')])


if __name__ == "__main__":
    main()
