
# -*- coding: utf-8 -*-
"""Produce the weather page's assets: five plain-sky backgrounds and one overlay.

    python tools/weather_prep.py
    python tools/weather_prep.py --preview

## Why two layers

The weather page started as five "overlay + sky" composites cross-fading into
each other. On hardware that had two problems:

1. **Layout**: the overlay was laid out on a square canvas, so on the round
   screen both ends of the forecast cards at the bottom were cut off, and the text
   at the top was too large. Baked into the image, the overlay could not be moved
   or scaled on its own.
2. **Motion**: the five cloud shapes differ a lot, and dissolving every 1.5 s kept
   the whole sky "morphing" -- watching it for a while made you dizzy. Real clouds
   drift slowly, and panning requires the sky and the overlay to be separate.

So it became:

    bottom  five plain skies (weather_cloud_0N.png); the firmware pans them slowly
            left and right, changing to the next only after each full sweep
    middle  one transparent overlay (drawn by this tool) that never moves
    top     the time and the Bluetooth symbol, drawn live by the firmware every
            frame (from the same time source as the watch face)

## Inputs and outputs

Inputs (src/Generate_3/):

    weather_cloud_01..05.png     plain-sky backgrounds, 468x468
    original/weather_ui_01.png   the original composite, only used to crop the forecast cards

Outputs:

    tools/assets_out/WTHRBG0..4.BIN     backgrounds, 468x468 RGB565
    tools/assets_out/WTHRUI.BIN         the overlay, UI_W x UI_H RGBA8888
    src/Generate_3/weather_overlay.png  the overlay as a PNG (to look at, and to pick in the GUI)

Then upload them with tools/asset_upload.py. The old WEATHER0..4.BIN (composites)
are no longer used and can be removed from eMMC with
`python tools/asset_upload.py --delete WEATHER0.BIN ...`.

## What the overlay contains

1. **The sun's glow**: in all five backgrounds the sun is at (234, 22). The sun
   would move with the pan, so a fixed white glow is laid over that same point to
   cover it -- only the clouds move.
2. **Text**: date, place, temperature, conditions, high / low. The text on the
   original could not be extracted cleanly (white text over white cloud is barely
   different from the background), so it is redrawn in Noto Sans TC with a blurred
   dark blue halo around it, in the style of the original.
3. **Forecast cards**: cropped whole from the original composite (the icons and
   lettering are the original's), given a rounded-corner mask, scaled down and
   moved up so both ends lie fully inside the round screen.

The time and the Bluetooth symbol are not in this layer: the time changes, so the
firmware draws it (see draw_weather() in render_task.c).

## Layout

Everything is centred horizontally. Vertical positions and sizes are the LAYOUT_*
values below. The forecast cards' corners must lie inside the circle of radius 234
with some margin, which requires

    sqrt((W/2 - R)^2 + (bottom - R - 234)^2) + R <= 234 - margin

Recompute when changing the cards' size or position.
"""
import os
import sys

try:
    from PIL import Image, ImageDraw, ImageFilter, ImageFont
except ImportError:
    print("Pillow is required: pip install pillow")
    sys.exit(1)

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import png_to_bin  # noqa: E402

GEN = os.path.normpath(os.path.join(HERE, "..", "src", "Generate_3"))
ORIG = os.path.join(GEN, "original")

FRAMES = 5
BG_SRC = "weather_cloud_%02d.png"
BG_TARGET = "WTHRBG%d.BIN"
CARD_SRC = "weather_ui_01.png"
UI_PNG = "weather_overlay.png"
UI_TARGET = "WTHRUI.BIN"

RES = 468
CX = RES // 2

#
# The overlay stores only the part with content, not the whole 468x468: both
# sides are largely transparent, and storing the whole frame would double what is
# read from PSRAM every frame. This box must match ASSET_WEATHER_UI_* in
# src/asset_store.h and WEATHER_UI_X/Y in render_task.c.
#
UI_X, UI_Y = 60, 0
UI_W, UI_H = 348, 392

#
# The font. Noto Sans TC is a variable font; the weight is chosen by name.
#
FONT_PATH = r"C:\Windows\Fonts\NotoSansTC-VF.ttf"

#
# Text: (string, size, weight, vertical centre). The vertical centre is the centre
# of the glyphs' actual ink, not of the line box.
#
LAYOUT_TEXT = [
    ("10月4日 週日",      20, "Medium",  114),
    ("台北",              26, "Medium",  146),
    ("32°",               78, "Regular", 196),
    ("局部多雲",          22, "Medium",  244),
    ("最高32°  最低25°",  17, "Medium",  270),
]

TEXT_COLOR = (255, 255, 255)

#
# The halo around the text: a blurred dark blue ring, as on the original.
#
HALO_COLOR = (16, 40, 80)
HALO_GROW = 3       # grow the glyphs outwards by this many px first
HALO_BLUR = 3.0     # then blur
HALO_ALPHA = 0.55

#
# The sun's glow: centre, fully opaque radius, and the radius where it fades to 0.
# The sun's core in the backgrounds has a radius of about 30 px and the pan is
# +-24 px (WEATHER_PAN_PX in render_task.c), so the core stays near the opaque
# area in every position, while the light around it drifts slightly with the
# clouds.
#
SUN_CX, SUN_CY = 234, 22
SUN_R_SOLID = 30
SUN_R_FADE = 110

#
# Forecast cards: their area on the original (corners included), the corner
# radius, and the new size and position.
#
CARD_SRC_BOX = (31, 332, 436, 446)     # x0, y0, x1, y1 (right and bottom exclusive)
CARD_SRC_R = 14
CARD_SCALE = 0.82
CARD_TOP = 290

SS = 4  # supersampling factor for text and masks


def font(size, weight):
    f = ImageFont.truetype(FONT_PATH, size * SS)
    f.set_variation_by_name(weight)
    return f


def text_mask(s, size, weight):
    """Return the L mask of one line of text, its ink tightly framed with a small
    border; the caller positions it by the mask's size."""
    f = font(size, weight)
    l, t, r, b = f.getbbox(s)
    w, h = r - l, b - t
    m = Image.new("L", (w + 8 * SS, h + 8 * SS), 0)
    ImageDraw.Draw(m).text((4 * SS - l, 4 * SS - t), s, font=f, fill=255)
    m = m.resize((m.width // SS, m.height // SS), Image.LANCZOS)
    return m


def paste_text(layer, s, size, weight, cy):
    """Draw one line of text centred on cy (the ink centre), halo included."""
    m = text_mask(s, size, weight)
    x = CX - m.width // 2
    y = int(round(cy - m.height / 2.0))

    full = Image.new("L", (RES, RES), 0)
    full.paste(m, (x, y))

    halo = full.filter(ImageFilter.MaxFilter(2 * HALO_GROW + 1))
    halo = halo.filter(ImageFilter.GaussianBlur(HALO_BLUR))
    halo = halo.point(lambda v: int(v * HALO_ALPHA))

    hl = Image.new("RGBA", (RES, RES), HALO_COLOR + (0,))
    hl.putalpha(halo)
    layer = Image.alpha_composite(layer, hl)

    tl = Image.new("RGBA", (RES, RES), TEXT_COLOR + (0,))
    tl.putalpha(full)
    return Image.alpha_composite(layer, tl)


def sun_glare(layer):
    """An opaque core, fading out to 0 with a smoothstep."""
    g = Image.new("L", (RES, RES), 0)
    px = g.load()
    for y in range(0, SUN_CY + SUN_R_FADE + 1):
        for x in range(SUN_CX - SUN_R_FADE, SUN_CX + SUN_R_FADE + 1):
            r = ((x - SUN_CX) ** 2 + (y - SUN_CY) ** 2) ** 0.5
            if r <= SUN_R_SOLID:
                a = 1.0
            elif r >= SUN_R_FADE:
                continue
            else:
                t = (SUN_R_FADE - r) / float(SUN_R_FADE - SUN_R_SOLID)
                a = t * t * (3.0 - 2.0 * t)
            px[x, y] = int(round(255 * a))
    gl = Image.new("RGBA", (RES, RES), (255, 255, 255, 0))
    gl.putalpha(g)
    return Image.alpha_composite(layer, gl)


def card(layer):
    """Crop the forecast cards from the original, apply the rounded mask, scale
    down, and paste centred at CARD_TOP."""
    src = Image.open(os.path.join(ORIG, CARD_SRC)).convert("RGB")
    x0, y0, x1, y1 = CARD_SRC_BOX
    c = src.crop(CARD_SRC_BOX)

    m = Image.new("L", (c.width * SS, c.height * SS), 0)
    ImageDraw.Draw(m).rounded_rectangle([0, 0, c.width * SS - 1, c.height * SS - 1],
                                        CARD_SRC_R * SS, fill=255)
    m = m.resize(c.size, Image.LANCZOS)

    rgba = c.convert("RGBA")
    rgba.putalpha(m)

    w = int(round(c.width * CARD_SCALE))
    h = int(round(c.height * CARD_SCALE))
    rgba = rgba.resize((w, h), Image.LANCZOS)

    full = Image.new("RGBA", (RES, RES), (0, 0, 0, 0))
    full.paste(rgba, (CX - w // 2, CARD_TOP))
    return Image.alpha_composite(layer, full), (CX - w // 2, CARD_TOP, w, h)


def build_overlay():
    layer = Image.new("RGBA", (RES, RES), (0, 0, 0, 0))
    layer = sun_glare(layer)
    for s, size, weight, cy in LAYOUT_TEXT:
        layer = paste_text(layer, s, size, weight, cy)
    layer, box = card(layer)
    return layer, box


def main(argv):
    print("Weather page assets: 5 backgrounds + overlay")

    for i in range(FRAMES):
        src = os.path.join(GEN, BG_SRC % (i + 1))
        ok, msg = png_to_bin.convert(src, BG_TARGET % i)
        if not ok:
            print("  %s: %s" % (BG_SRC % (i + 1), msg))
            return 1

    layer, (cx, cy, cw, ch) = build_overlay()

    #
    # Make sure the crop box does not cut off any opaque pixel.
    #
    bbox = layer.getchannel("A").getbbox()
    if (bbox[0] < UI_X or bbox[1] < UI_Y or
            bbox[2] > UI_X + UI_W or bbox[3] > UI_Y + UI_H):
        print("  the overlay's content %s exceeds the crop box (%d,%d %dx%d); enlarge UI_W/UI_H"
              % (bbox, UI_X, UI_Y, UI_W, UI_H))
        return 1

    ui = layer.crop((UI_X, UI_Y, UI_X + UI_W, UI_Y + UI_H))
    out_png = os.path.join(GEN, UI_PNG)
    ui.save(out_png)
    print("  %-24s %dx%d (content %s, cards x %d..%d y %d..%d)"
          % (UI_PNG, UI_W, UI_H, bbox, cx, cx + cw, cy, cy + ch))

    ok, msg = png_to_bin.convert(out_png, UI_TARGET)
    if not ok:
        print("  %s: %s" % (UI_PNG, msg))
        return 1

    if "--preview" in argv:
        bg = Image.open(os.path.join(GEN, BG_SRC % 1)).convert("RGBA")
        comp = Image.alpha_composite(bg, layer)
        path = os.path.join(GEN, "weather_overlay_preview.png")
        comp.convert("RGB").save(path)
        print("  preview -> %s" % path)

    print()
    print("Next, send them to eMMC with tools/asset_upload.py (\"Add assets_out\" in the GUI).")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
