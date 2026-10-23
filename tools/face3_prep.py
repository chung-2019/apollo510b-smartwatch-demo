"""Convert the third watch face's (blue numeral) assets into .BIN files and compose a preview.

    python tools/face3_prep.py
    python tools/face3_prep.py --preview

The full explanation (including how every number was measured) is in the "third asset set"
section of docs/ui_design/WATCHFACE_SELECTOR.md.

## Assets and outputs

    Watch_face/face2_3D_dial_454x454.png -> WF3BG.BIN   454x454
    1.rgba.h  -> WF3HR.BIN    23x107   pivot (11, 144)
    2.rgba.h  -> WF3MIN.BIN   33x178   pivot (16, 162)
    3.rgba.h  -> WF3SEC.BIN   18x213   pivot ( 9, 144)

The hands share their artwork with the second watch face but are separate assets: those
three were drawn for a 390 px dial while this face is 454, and each was tuned further.

## Three adjustments (every number is measured, not estimated)

1. **Dial centre -> asset centre**. The source image's centre is at (221.6, 216.7), 4.9
   left and 9.8 up. The hands rotate about the asset's centre, and without centring their
   meeting point falls outside the knob. The centring happens in convert() in
   png_to_bin.py (AUTO_CENTER) rather than in this tool -- the GUI's "Add image / asset"
   button calls that one directly, and centring written only here would be bypassed.

2. **Hand tail -> against the knob's outer ring**. HOUR_TAIL_TARGET; only the hour hand
   leaves a gap (the minute and second tails are negative, crossing the centre). The fix
   lengthens the shaft rather than moving the pivot.

3. **Hand tip -> clear of the numerals**. HAND_TIP_TARGET; the numerals sit at r = 146..163.
   Shortening scales the whole hand proportionally rather than cropping the tip (cropping flattens the point).

Points 2 and 3 interact: proportional scaling shrinks the tail too, so extend_hour() has to
take an extra parameter and compute the extension from the final factor.

## Do not take the wrong dial

face2_3D_numbers_454x454.png in the same directory is a **design reference** (the dial
already has the hands painted on). Used as an asset it gives two sets of hands, one of
which never moves. The target values above were measured from it.

## Three places to keep in sync after a change

    src/asset_store.h    ASSET_FACE3_*_W/H
    src/render_task.c    PIVOT_F3_* (stored negated)
    tools/png_to_bin.py  TARGETS

A length mismatch makes the firmware treat it as a short read and that hand is not drawn at all.
"""
import os
import sys

try:
    from PIL import Image
except ImportError:
    print("Pillow is required: pip install pillow")
    sys.exit(1)

import png_to_bin
import face2_prep

HERE = os.path.dirname(os.path.abspath(__file__))
GEN = os.path.join(HERE, "..", "src", "generated")

DIAL_SRC = os.path.join("Watch_face", "face2_3D_dial_454x454.png")
DIAL_TARGET = "WF3BG.BIN"

#
# The centred dial, saved back into src/generated as a PNG.
#
# Why keep this intermediate file: centring used to happen only inside this tool, but the
# "Add image / asset" button in build_gui.py calls png_to_bin itself -- that path does no
# centring and would overwrite the .BIN produced here, with nothing to show for it. With
# the centred result frozen into a PNG, any conversion route is correct as long as its
# source points at it.
#
DIAL_CENTERED = "face3_dial_454_centered.png"

#
# The dial's width / the width of the dial those three hands were drawn for.
#
# The upscale happens here, so the exported .BIN is already at the 454 scale and the
# firmware needs no factor at all -- the hands and the dial share one basis and
# face_render() gets by with a single fScale.
#
# Upscaling on the PC has another benefit: LANCZOS is sharper than the GPU's bilinear and
# runs once rather than resampling every frame. The cost is another copy of the hands in
# SSRAM (about 57 KB), not shared with the second watch face.
#
HAND_SCALE = 454.0 / 390.0

#
# The upscaled hands. Both the sizes and the pivots are the originals multiplied by
# HAND_SCALE and rounded -- the two must use the same factor, since scaling the asset
# without the pivot makes the hand grow downward rather than longer.
#
# The pivot values have to match PIVOT_F3_* in render_task.c (which stores them negated).
# Change the factor and this has to be re-run and those synchronized; --preview prints the computed values.
#
HAND_TARGETS = [
    ("1.rgba.h", "WF3HR.BIN"),
    ("2.rgba.h", "WF3MIN.BIN"),
    ("3.rgba.h", "WF3SEC.BIN"),
]


#
# The hour hand's shaft has to be lengthened.
#
# The central knob on this face is much smaller than aurora's (visual edge 29 px vs 36 px,
# relative radius 0.128 vs 0.185), while the hands' proportions follow aurora. The minute
# and second pivots are both inside their assets with the tails crossing the centre, so
# they still cover it; only the hour tail stops 42 px above the pivot, 13 px further out
# than the knob's edge -- on hardware that is a visible gap between the hour hand and the knob.
#
# The fix lengthens the shaft rather than shortening the whole hand: moving the pivot would
# pull the tip back too and throw the hour hand's proportions off. Lengthening only touches the end nobody sees.
#
# The insertion point is y=100: the asset's last 3 rows are the finishing shape (with
# transparent rounded corners), and simply duplicating the last row would stretch that shape
# into a line. Inserting before the finish lengthens the constant-width middle section, and the seam is invisible.
#
HOUR_EXTEND_AFTER = 100

#
# How far the tail should stay from the centre (px, at the 454 scale).
#
# 42 was measured from face2_3D_numbers_454x454.png -- the design reference for this face,
# with the hands already drawn. Taking a brightness profile along the hour hand: r=33..41
# is the black ring around the knob, and the hand's silver starts at r=42. That is, the
# hour hand should sit right against the outside of the black ring without overlapping.
#
# 42 happens to equal the original asset's tail (163 - 121), so the extension extend_hour()
# computes is 0, i.e. no extension. The constant stays to record "the measured target"
# rather than leaving someone to guess why the asset's height is 121.
#
# Changing it to 24 (lengthening the shaft) at one point was a misdiagnosis: the gap seen
# on hardware then actually came from the dial on eMMC not being centred yet (its centre
# was 5,10 up and left), so the hands rotating about the asset's centre were pulled away.
# With the dial centred, 42 is flush as it was.
#
HOUR_TAIL_TARGET = 38


def extend_hour(img, pivot_y, extra=1.0):
    """Lengthen the hour hand's shaft and return (image, new original height).

    Done at the original scale, with the upscale left to the next step -- extending first
    lets LANCZOS smooth the seam along with everything else.

    extra is the additional factor used to shorten the tip. It has to be included: shrinking
    shrinks the tail too, and without compensating the tail retreats from the knob's outer edge into the black ring.
    """
    w, h = img.size

    #
    # The target height is derived from "the scaled pivot" and "where the tail should be", then converted back to the original scale.
    #
    s = HAND_SCALE * extra
    py_scaled = pivot_y * s
    h_target = int(round((py_scaled - HOUR_TAIL_TARGET) / s))

    n = h_target - h
    if n <= 0:
        return img, h

    out = Image.new("RGBA", (w, h_target), (0, 0, 0, 0))

    #
    # Everything before the finish is copied as-is, then row HOUR_EXTEND_AFTER is duplicated
    # n times, and finally the original finish is appended.
    #
    out.paste(img.crop((0, 0, w, HOUR_EXTEND_AFTER + 1)), (0, 0))

    row = img.crop((0, HOUR_EXTEND_AFTER, w, HOUR_EXTEND_AFTER + 1))
    for i in range(n):
        out.paste(row, (0, HOUR_EXTEND_AFTER + 1 + i))

    out.paste(img.crop((0, HOUR_EXTEND_AFTER + 1, w, h)),
              (0, HOUR_EXTEND_AFTER + 1 + n))

    return out, h_target


#
# How far from the centre each hand's tip should stop (px, at the 454 scale). None follows HAND_SCALE.
#
# The numerals on this face sit at r = 146..163, and a hand that is too long covers them.
# 144 stops the tip 2 px inside the numerals' inner edge.
#
# The reference image (face2_3D_numbers_454x454.png) measures tips at 157 / 161 / 200 --
# its hands do overlap the numerals, so that is not copied here and the tips are kept clear
# instead. The minute hand is left as it is: its length has never been reported as a problem, and changing it would diverge further from the reference.
#
# Shortening uses a **proportional scale of the whole hand** rather than cropping the tip:
# cropping flattens the point, while scaling takes the pivot with it and preserves the
# tail's relationship to the knob (the hour tail goes from 38 to 37 and is still flush).
#
HAND_TIP_TARGET = (144, None, 144)


def scaled(size, pivot, extra=1.0):
    """Multiply the size and the pivot by HAND_SCALE (optionally times extra)."""
    s = HAND_SCALE * extra

    w = int(round(size[0] * s))
    h = int(round(size[1] * s))
    px = int(round(pivot[0] * s))
    py = int(round(pivot[1] * s))

    return (w, h), (px, py)


def hand_scaled(idx, name, size, pivot):
    """Get one upscaled hand and return (image, (w,h), (px,py)).

    The hour hand (idx 0) has its shaft lengthened before being scaled; see extend_hour().
    """
    img = face2_prep.hand_image(name, size)
    h_src = size[1]

    #
    # With a tip target given, the extra scale is derived from it: the tip's distance from the centre is the scaled pivot Y.
    #
    # It has to come before lengthening the shaft, which needs the final factor to compute the extension.
    #
    extra = 1.0
    tip = HAND_TIP_TARGET[idx]
    if tip is not None:
        extra = float(tip) / (pivot[1] * HAND_SCALE)

    if idx == 0:
        img, h_src = extend_hour(img, pivot[1], extra)

    (nw, nh), (npx, npy) = scaled((size[0], h_src), pivot, extra)

    return img.resize((nw, nh), Image.LANCZOS), (nw, nh), (npx, npy)


def preview(out_path):
    """The dial plus the three upscaled hands, to confirm the factor is right."""
    hh, mm, ss = face2_prep.PREVIEW_TIME

    dial, dx, dy, (ocx, ocy) = png_to_bin.center_dial(
        Image.open(os.path.join(GEN, DIAL_SRC)).convert("RGB"))

    print("  dial centre (%.1f, %.1f) -> shifted (%+d, %+d) to centre" % (ocx, ocy, dx, dy))

    bg = dial.convert("RGBA")
    w, h = bg.size
    cx, cy = w / 2.0, h / 2.0

    angles = [
        (hh + mm / 60.0) / 12.0 * 360.0,
        mm / 60.0 * 360.0,
        ss / 60.0 * 360.0,
    ]

    for idx, ((name, _, size, pivot), angle) in enumerate(
            zip(face2_prep.HANDS, angles)):
        hand, (nw, nh), (npx, npy) = hand_scaled(idx, name, size, pivot)

        layer = Image.new("RGBA", (w, h), (0, 0, 0, 0))
        layer.paste(hand, (int(round(cx - npx)), int(round(cy - npy))), hand)
        layer = layer.rotate(-angle, resample=Image.BICUBIC, center=(cx, cy))

        bg = Image.alpha_composite(bg, layer)

        print("  %-10s %2dx%-3d -> %2dx%-3d  pivot (%d, %d)  tail %+d"
              % (name, size[0], size[1], nw, nh, npx, npy, npy - nh))

    bg.save(out_path)

    print("  preview -> %s (%02d:%02d:%02d, hands x%.3f)"
          % (out_path, hh, mm, ss, HAND_SCALE))


def main(argv):
    if "--preview" in argv:
        preview(os.path.join(GEN, "face3_preview.png"))
        return 0

    os.makedirs(png_to_bin.OUT_DIR, exist_ok=True)

    #
    # The dial. png_to_bin.convert() does the centring itself (see AUTO_CENTER there), so
    # this follows the same path as every other conversion route.
    #
    # A centred PNG is also saved: for human eyes, and to give "convert this one and it is
    # certainly right" a file to point at. The production flow does not read it.
    #
    dial, dx, dy, (ocx, ocy) = png_to_bin.center_dial(
        Image.open(os.path.join(GEN, DIAL_SRC)).convert("RGB"))
    dial.save(os.path.join(GEN, DIAL_CENTERED))

    print("  %-26s centre (%.1f, %.1f) shifted (%+d, %+d)"
          % (DIAL_CENTERED, ocx, ocy, dx, dy))

    ok, msg = png_to_bin.convert(os.path.join(GEN, DIAL_SRC), DIAL_TARGET)
    if not ok:
        print("  %s: %s" % (DIAL_SRC, msg))
        return 1

    for idx, ((name, _, size, pivot), (_, target)) in enumerate(
            zip(face2_prep.HANDS, HAND_TARGETS)):
        hand, (nw, nh), (npx, npy) = hand_scaled(idx, name, size, pivot)

        #
        # The byte order matches png_to_bin.to_rgba() (R, G, B, A), so tobytes() goes straight out.
        #
        data = hand.convert("RGBA").tobytes()

        with open(os.path.join(png_to_bin.OUT_DIR, target), "wb") as f:
            f.write(data)

        print("  %-14s %8d bytes  %dx%-3d RGBA  pivot (%d,%d)  tail %+d%s"
              % (target, len(data), nw, nh, npx, npy, npy - nh,
                 "  <- shaft lengthened" if (idx == 0 and nh > int(round(size[1] * HAND_SCALE))) else ""))

    print("  converted 4 assets -> %s" % png_to_bin.OUT_DIR)
    print("  the pivots must match PIVOT_F3_* in render_task.c (stored negated there)")
    print("  then send them to the board's eMMC with asset_upload.py")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
