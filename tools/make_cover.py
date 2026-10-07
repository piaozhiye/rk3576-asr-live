#!/usr/bin/env python3
# Cover for the RK3576 on-device subtitle article.
#
# Drawn rather than photographed, because the board's screenshot path needs a
# privileged weston client ("capture error: unauthorized") and there is no
# frbgrab or kmsgrab on the image.  So this is a diagram of the thing rather
# than a picture of it: a portrait panel with a video area and a subtitle strip
# on top, which is exactly the layout the overlay ends up producing on screen.
#
#   python3 tools/make_cover.py [out.png]

import os
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

W, H = 900, 1200                      # 3:4, the usual WeChat cover ratio
BG_TOP = (14, 18, 30)
BG_BOT = (8, 10, 18)
ACCENT = (86, 214, 168)               # the green the feasibility probe drew
WARN = (255, 176, 92)
DIM = (120, 132, 158)


def vgrad(w, h, top, bot):
    """Vertical gradient, built row by row; cheaper than a per-pixel loop."""
    img = Image.new("RGB", (1, h))
    px = img.load()
    for y in range(h):
        t = y / max(1, h - 1)
        px[0, y] = (int(top[0] + (bot[0] - top[0]) * t),
                    int(top[1] + (bot[1] - top[1]) * t),
                    int(top[2] + (bot[2] - top[2]) * t))
    return img.resize((w, h), Image.BILINEAR)


def cjk_font(px, weight="regular"):
    """A font that can actually draw the subtitle line.

    The board only has Source Han Sans CN; the host has hundreds, and picking
    the wrong one renders every han character as a tofu box -- which is the
    exact failure the overlay had to fix, so it is worth being deliberate here.
    """
    cands = [
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc",
        "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
        "/usr/share/fonts/truetype/arphic/uming.ttc",
    ]
    for c in cands:
        if os.path.exists(c):
            try:
                return ImageFont.truetype(c, px)
            except OSError:
                pass
    # Fall back to whatever claims to have CJK coverage.
    import subprocess
    try:
        out = subprocess.run(["fc-list", ":lang=zh-cn", "file", "family"],
                             capture_output=True, text=True, timeout=10).stdout
    except Exception:
        out = ""
    for line in out.splitlines():
        path = line.split(":")[0].strip()
        if path and os.path.exists(path):
            try:
                return ImageFont.truetype(path, px)
            except OSError:
                continue
    return ImageFont.load_default()


def latin_font(px, bold=False):
    name = "DejaVuSans-Bold.ttf" if bold else "DejaVuSans.ttf"
    for d in ("/usr/share/fonts/truetype/dejavu", "/usr/share/fonts/dejavu"):
        p = os.path.join(d, name)
        if os.path.exists(p):
            try:
                return ImageFont.truetype(p, px)
            except OSError:
                pass
    return ImageFont.load_default()


def glow(img, draw_fn, radius=18, alpha=90):
    """Draw on a separate layer and blur it, for a soft neon edge."""
    layer = Image.new("RGBA", img.size, (0, 0, 0, 0))
    draw_fn(ImageDraw.Draw(layer))
    layer = layer.filter(ImageFilter.GaussianBlur(radius))
    out = img.convert("RGBA")
    out.alpha_composite(layer)
    return out.convert("RGB")


def main():
    out_path = sys.argv[1] if len(sys.argv) > 1 else "rk3576-subtitle-cover.png"
    img = vgrad(W, H, BG_TOP, BG_BOT)
    d = ImageDraw.Draw(img)

    # Faint grid, to read as a device rather than a slide.
    for x in range(0, W, 45):
        d.line([(x, 0), (x, H)], fill=(22, 27, 42), width=1)
    for y in range(0, H, 45):
        d.line([(0, y), (W, y)], fill=(22, 27, 42), width=1)

    f_h1 = cjk_font(74)
    f_h2 = cjk_font(34)
    f_sub = latin_font(25, bold=True)
    f_cap = cjk_font(28)
    f_tag = latin_font(21, bold=True)

    # --- headline, top-left, with the model name in the accent colour --------
    d.text((70, 96), "实时字幕", font=f_h1, fill=(238, 242, 250))
    d.text((70, 190), "全在端侧", font=f_h1, fill=ACCENT)
    d.text((74, 296), "RK3576", font=f_sub, fill=DIM)
    d.text((74, 296), "", font=f_sub)

    # --- the panel ---------------------------------------------------------
    px0, py0, pw, ph = 96, 372, 708, 600
    px1, py1 = px0 + pw, py0 + ph
    d.rounded_rectangle([px0 - 3, py0 - 3, px1 + 3, py1 + 3], 18,
                        outline=(46, 54, 76), width=2)
    d.rounded_rectangle([px0, py0, px1, py1], 15, fill=(17, 20, 30))

    # Video area: abstract bands standing in for a frame, kept below the strip
    # so they are not hidden by it.  Deliberately not a photograph -- the board
    # cannot be screenshotted without a privileged weston client, and a
    # stand-in that could be mistaken for real footage would be worse than none.
    # vy1 is the bottom of the subtitle strip: 320 px of 1080x1920, which is
    # the proportion the overlay ended up at -- a band tall enough for three
    # rows that does not eat the video.  It has to be defined before the video
    # area below uses it as its top edge, which is why it is here and not next
    # to the strip itself.
    vy1 = py0 + 200
    vy0 = vy1 + 6                      # just under the subtitle strip
    vh = py1 - vy0 - 60
    for i in range(6):
        t = i / 5.0
        yy = vy0 + 26 + int(t * (vh - 40))
        a = int(20 - 13 * t)                     # fades toward the bottom
        d.rounded_rectangle([px0 + 46, yy, px1 - 46 - int(90 * t), yy + a], 4,
                            fill=(42, 50, 74))
    # No play triangle: it sat on top of the bands and read as clutter.  The
    # bands alone already say "video area" once the subtitle strip above says
    # "subtitles", and the pair is the point of the drawing.

    # The subtitle strip: the thing this whole build is about.
    strip = [px0 + 14, py0 + 14, px1 - 14, vy1 - 14]
    img = glow(img, lambda dd: dd.rounded_rectangle(strip, 10,
                                                    outline=ACCENT + (150,), width=3),
               radius=16)
    d = ImageDraw.Draw(img)
    d.rounded_rectangle(strip, 10, fill=(20, 34, 34), outline=ACCENT, width=2)
    d.text((strip[0] + 22, strip[1] + 20), "说话过程中字幕实时生长",
           font=f_cap, fill=(226, 240, 236))
    d.text((strip[0] + 22, strip[1] + 74),
           "句尾自动精修", font=f_cap, fill=(226, 240, 236))

    # the two compute paths, drawn as two lanes into the panel
    def lanes(dd):
        dd.line([(150, 1120), (330, 1088), (330, 1042)], fill=ACCENT + (170,), width=4)
        dd.line([(430, 1120), (570, 1088), (570, 1042)], fill=WARN + (170,), width=4)
    img = glow(img, lanes, radius=12)
    d = ImageDraw.Draw(img)

    d.rounded_rectangle([92, 1082, 250, 1142], 10, fill=(24, 34, 36),
                        outline=ACCENT, width=2)
    d.text((112, 1097), "CPU", font=f_tag, fill=ACCENT)
    d.text((166, 1101), "流式", font=f_cap, fill=(206, 226, 220))

    d.rounded_rectangle([492, 1082, 660, 1142], 10, fill=(38, 30, 22),
                        outline=WARN, width=2)
    d.text((512, 1097), "NPU", font=f_tag, fill=WARN)
    d.text((572, 1101), "离线", font=f_cap, fill=(240, 224, 204))

    # 6 TOPS, the one number worth putting on the cover
    d.text((W - 70, 1105), "6 TOPS", font=latin_font(30, bold=True),
           fill=(96, 106, 130), anchor="rs")

    d.text((70, 66), "ON-DEVICE SUBTITLING", font=latin_font(20, bold=True),
           fill=(84, 94, 118))
    d.line([(70, 108), (196, 108)], fill=ACCENT, width=3)

    img.save(out_path)
    print(f"wrote {out_path}  {W}x{H}")


if __name__ == "__main__":
    main()
