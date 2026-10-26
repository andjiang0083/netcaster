#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""make-cover.py — NETCASTER 商店封面（1280×720）

规范：skill `m5burner-store-art`（M5Burner 商店产品图规范）
  * 文字底线 y ≤ 561（平台底部黑幕 + 详情页固件名落点）
  * 左上 300×110 让给平台版本徽标
  * 图内不印版本号；中英双语都在图内
  * 主视觉 = 本固件真实渲染帧（sim/ 产物），整数倍最近邻放大
  * 文案唯一真源 = ~/esp32/tools/covers.json 的 NETCASTER 条目

用法：python3 tools/make-cover.py
产物：releases/cover-1280x720-netcaster.png (+ .texts.json)
"""
import json
import os
import sys

sys.path.insert(0, os.path.expanduser("~/esp32/tools"))
from mkcover import gate_texts, _ledger  # noqa: E402

from PIL import Image, ImageDraw, ImageFilter, ImageFont  # noqa: E402

PROJ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
COVERS = os.path.expanduser("~/esp32/tools/covers.json")
SIM = os.path.join(PROJ, "sim")
OUT = os.path.join(PROJ, "releases")

W, H = 1280, 720
S = 3                       # 超采样倍率
CJK = "/System/Library/Fonts/Hiragino Sans GB.ttc"
BOLD = "/System/Library/Fonts/Supplemental/Arial Bold.ttf"
REG = "/System/Library/Fonts/Supplemental/Arial.ttf"

BG = (7, 12, 18)            # 深海黑蓝
WHITE = (238, 246, 250)
GREY = (140, 158, 170)


def font(path, size, index=0):
    try:
        return ImageFont.truetype(path, size, index=index)
    except Exception:
        return ImageFont.truetype(REG, size)


def load_frame(name, sc=2):
    """真实渲染帧（sim/frame_*_4x.png → 整数倍降到 sc×）。
    240×135 的 sc=2 → 480×270。"""
    src = os.path.join(SIM, f"{name}_4x.png")
    im = Image.open(src).convert("RGB")
    step = 4 // sc
    if step > 1:
        im = im.resize((im.width // step, im.height // step), Image.NEAREST)
    return im


def keypad(w, h, acc):
    """Cardputer 键盘区：5 行 × 14 列键帽（示意，不必逐键对应）。"""
    kp = Image.new("RGB", (w, h), (30, 32, 38))
    kd = ImageDraw.Draw(kp)
    rows, cols = 5, 14
    mx, my = 8, 6
    cw = (w - mx * 2) / cols
    ch = (h - my * 2) / rows
    for r in range(rows):
        for c in range(cols):
            x0 = mx + c * cw
            y0 = my + r * ch
            kd.rounded_rectangle([x0 + 1, y0 + 1, x0 + cw - 2, y0 + ch - 2],
                                 radius=2, fill=(44, 47, 55), outline=(22, 24, 28))
            kd.rounded_rectangle([x0 + 3, y0 + 2, x0 + cw - 4, y0 + ch - 4],
                                 radius=2, fill=(52, 56, 65))
    # 屏幕下方一条状态灯带（呼应真机的提示行）
    kd.rectangle([0, 0, w, 2], fill=acc)
    return kp


def compose(scale):
    cover = json.load(open(COVERS, encoding="utf-8"))["NETCASTER"]
    accent = cover.get("accent", "#2BD9D9")
    acc = tuple(int(accent.lstrip("#")[i:i + 2], 16) for i in (0, 2, 4))

    w3, h3 = W * S, H * S
    img = Image.new("RGB", (w3, h3), BG)

    # 背景：右下角一团深海青光（主题色）
    glow = Image.new("RGB", (w3, h3), BG)
    ImageDraw.Draw(glow).ellipse([int(w3 * 0.45), int(-h3 * 0.18), int(w3 * 1.15), int(h3 * 1.20)],
                                 fill=(10, 34, 40))
    img = Image.blend(img, glow.filter(ImageFilter.GaussianBlur(90 * S // 3)), 0.9)
    d = ImageDraw.Draw(img, "RGBA")

    x0 = 62
    texts = []

    def txt(xy, s, f, fill):
        fnt = font(f[0], f[1] * S, f[2] if len(f) > 2 else 0)
        b = fnt.getbbox(s)
        texts.append({"s": s, "kind": "text", "bbox": [
            int(xy[0] + b[0] / S), int(xy[1] + b[1] / S),
            int(xy[0] + b[2] / S), int(xy[1] + b[3] / S)]})
        d.text((xy[0] * S - b[0], xy[1] * S - b[1]), s, font=fnt, fill=fill)
        return (b[2] - b[0]) / S

    txt((x0, 112), cover["title"], (BOLD, 92), WHITE)
    y = 238
    txt((x0, y), cover["sub"], (CJK, 32), acc)
    y += 62
    for b in cover["bullets"][:4]:
        d.ellipse([x0 * S, (y + 7) * S, (x0 + 9) * S, (y + 16) * S], fill=acc)
        txt((x0 + 24, y), b, (CJK, 25), WHITE)
        y += 40
    txt((x0, y + 24), cover.get("sub_en", ""), (BOLD, 19), GREY)
    txt((x0, y + 56), cover.get("device", ""), (BOLD, 17), GREY)

    n_text = gate_texts("NETCASTER", texts)
    print(f"  版面门禁：{n_text} 条文字全部在安全区内 ✓")

    img = img.resize((W * scale, H * scale), Image.BOX)
    base = img

    # ---------- 机身：Cardputer（屏 + 键盘），屏幕保持像素锐利 ----------
    scr = load_frame("frame_3_catch", 2)          # 捕获瞬间，最抓眼
    pad = 14
    kb_h = 104
    bez_w = scr.width + pad * 2
    bez_h = scr.height + pad * 2 + 8 + kb_h

    bez = Image.new("RGB", (bez_w, bez_h), (26, 27, 32))
    bd = ImageDraw.Draw(bez)
    bd.rounded_rectangle([0, 0, bez_w - 1, bez_h - 1], radius=18,
                         fill=(28, 29, 34), outline=(62, 66, 74), width=1)
    bd.rounded_rectangle([pad - 4, pad - 4, pad + scr.width + 3, pad + scr.height + 3],
                         radius=6, outline=(12, 13, 16), width=2)
    bez.paste(scr, (pad, pad))
    bez.paste(keypad(scr.width, kb_h, acc), (pad, pad + scr.height + 8))
    bd.rounded_rectangle([0, 0, bez_w - 1, bez_h - 1], radius=18,
                         outline=(62, 66, 74), width=1)

    fx, fy = int(700 * scale), int(96 * scale)
    assert fx >= 56 * scale and fx + bez.width <= (W - 56) * scale, "机身越左右安全边"
    assert fy + bez.height <= 561 * scale, "机身压到平台黑幕区"

    sh = Image.new("RGBA", base.size, (0, 0, 0, 0))
    ImageDraw.Draw(sh).rounded_rectangle(
        [fx + 8, fy + 12, fx + bez.width + 8, fy + bez.height + 12],
        radius=20, fill=(0, 0, 0, 170))
    base = Image.alpha_composite(base.convert("RGBA"),
                                 sh.filter(ImageFilter.GaussianBlur(11 * scale))).convert("RGB")
    base.paste(bez, (fx, fy))
    return base, texts


def main():
    os.makedirs(OUT, exist_ok=True)
    im, texts = compose(1)
    p = os.path.join(OUT, "cover-1280x720-netcaster.png")
    im.save(p)
    _ledger(os.path.splitext(p)[0] + ".texts.json",
            {"name": "NETCASTER", "_project": "netcaster"}, p, "frame",
            {"kind": "frame", "path": "sim/frame_3_catch_4x.png",
             "px": [240, 135], "scale": 2}, texts)
    print(f"→ {p}  {im.size}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
