#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""make-cover-photo.py — NETCASTER 商店封面（1280×720，真机实拍版）

规范：skill `m5burner-store-art`
  * 文字底线 y ≤ 561（平台底部黑幕 + 详情页固件名落点）
  * 左上 300×110 让给平台版本徽标 → 标题从 y=118 起、x=330 起
  * 图内不印版本号；中英双语都在图内
  * 主视觉 = 真机实拍（Cardputer ADV 运行照），16:9 裁切 + LANCZOS
  * 文案唯一真源 = ~/esp32/tools/covers.json 的 NETCASTER 条目

用法：python3 tools/make-cover-photo.py <实拍.jpg>
产物：releases/cover-1280x720-netcaster.png (+ .texts.json)
"""
import json
import os
import sys

sys.path.insert(0, os.path.expanduser("~/esp32/tools"))
from mkcover import gate_texts, _ledger  # noqa: E402

from PIL import Image, ImageDraw, ImageFont  # noqa: E402

PROJ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
COVERS = os.path.expanduser("~/esp32/tools/covers.json")
OUT = os.path.join(PROJ, "releases")

W, H = 1280, 720
S = 3
CJK = "/System/Library/Fonts/Hiragino Sans GB.ttc"
BOLD = "/System/Library/Fonts/Supplemental/Arial Bold.ttf"
REG = "/System/Library/Fonts/Supplemental/Arial.ttf"

WHITE = (245, 250, 252)
GREY = (172, 190, 200)


def font(path, size, index=0):
    try:
        return ImageFont.truetype(path, size, index=index)
    except Exception:
        return ImageFont.truetype(REG, size)


def crop_16x9(im):
    """全宽 16:9：保留整块屏幕 + 机身 + 一段键盘，窗口略偏上留标题带。"""
    w, h = im.size
    ch = int(w * H / W)
    y0 = max(0, min(h - ch, int(h * 0.105)))
    return im.crop((0, y0, w, y0 + ch)).resize((W, H), Image.LANCZOS)


def compose(photo, scale=1):
    cover = json.load(open(COVERS, encoding="utf-8"))["NETCASTER"]
    accent = cover.get("accent", "#2BD9D9")
    acc = tuple(int(accent.lstrip("#")[i:i + 2], 16) for i in (0, 2, 4))

    base = crop_16x9(Image.open(photo).convert("RGB"))
    w3, h3 = W * S, H * S
    base = base.resize((w3, h3), Image.LANCZOS).convert("RGBA")

    # 顶部渐隐暗带：实拍上沿是笔记本键盘的背光键帽，不压暗白字读不清
    ov = Image.new("RGBA", (w3, h3), (0, 0, 0, 0))
    od = ImageDraw.Draw(ov)
    # 一条贯穿左右的半透明深色带：文字压在它上面，背景再杂也读得清；
    # 下沿再渐隐一段，避免和真机画面之间出现硬边。
    band = int(h3 * 0.30)
    od.rectangle([0, 0, w3, band], fill=(4, 8, 13, 216))
    fade = int(h3 * 0.10)
    for i in range(fade):
        t = i / fade
        a = int(216 * (1 - t) ** 1.4)
        od.line([(0, band + i), (w3, band + i)], fill=(4, 8, 13, a))
    base = Image.alpha_composite(base, ov)

    d = ImageDraw.Draw(base, "RGBA")
    texts = []

    def txt(xy, s, f, fill):
        fnt = font(f[0], f[1] * S, f[2] if len(f) > 2 else 0)
        b = fnt.getbbox(s)
        texts.append({"s": s, "kind": "text", "bbox": [
            int(xy[0] + b[0] / S), int(xy[1] + b[1] / S),
            int(xy[0] + b[2] / S), int(xy[1] + b[3] / S)]})
        # 描边：缩略图下细笔画的边界全靠它撑住
        d.text((xy[0] * S - b[0], xy[1] * S - b[1]), s, font=fnt, fill=fill,
               stroke_width=max(2, int(f[1] * S * 0.035)), stroke_fill=(2, 5, 9))
        return (b[2] - b[0]) / S

    # 让开左上 300×110 的版本徽标区，标题从 x=330 起
    # 三行全部落在顶部实心暗带内（y≈86~215），不碰屏幕和白色机身
    txt((330, 62), cover["title"], (BOLD, 78), WHITE)
    txt((332, 156), cover["sub"], (CJK, 36, 1), WHITE)
    txt((332, 208), cover.get("sub_en", ""), (BOLD, 26), (176, 240, 244))

    n = gate_texts("NETCASTER", texts)
    print(f"  版面门禁：{n} 条文字全部在安全区内 ✓")

    return base.resize((W * scale, H * scale), Image.BOX).convert("RGB"), texts


def main():
    if len(sys.argv) < 2:
        print("用法：python3 tools/make-cover-photo.py <实拍.jpg>")
        return 2
    photo = sys.argv[1]
    os.makedirs(OUT, exist_ok=True)
    im, texts = compose(photo, 1)
    p = os.path.join(OUT, "cover-1280x720-netcaster.png")
    im.save(p)
    _ledger(os.path.splitext(p)[0] + ".texts.json",
            {"name": "NETCASTER", "_project": "netcaster"}, p, "photo",
            {"kind": "photo", "path": os.path.relpath(photo, PROJ),
             "px": [W, H], "scale": 1}, texts)
    print(f"→ {p}  {im.size}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
