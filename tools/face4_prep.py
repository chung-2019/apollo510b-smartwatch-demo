"""Convert the fourth watch face's (Ambiq Apollo510B) assets into .BIN files and compose a preview.

    python tools/face4_prep.py
    python tools/face4_prep.py --preview

The full explanation is in docs/ui_design/WATCHFACE_AMBIQ_FACE4.md.

## Assets and outputs

The source is src/generated/Ambiq_Watch_face/, a set already packed by
tools/split_ambiq_watchface_assets.py (a 454x454 RGB dial and three hands each cropped
tight and centred in its own canvas), so this tool needs none of the scaling or shaft
surgery face3 requires -- those were needed because its hands were drawn for a 390 dial.

    Ambiq_Apollo510B_dial_454x454.png    -> WF4BG.BIN   454x454 RGB565
    Ambiq_Apollo510B_hour_30x132.png     -> WF4HR.BIN    30x132  RGBA  pivot (15, 109)
    Ambiq_Apollo510B_minute_30x205.png   -> WF4MIN.BIN   30x205  RGBA  pivot (15, 172)
    Ambiq_Apollo510B_second_18x126.png   -> WF4SEC.BIN   18x126  RGBA  pivot ( 9, 125)

## Two things this tool does that the source images do not

### 1. Repair the hand ghost on the dial

The "no hands" dial is not actually clean: a vertical silver ghost runs through the knob at
the centre, about 80 px above and below (see ARTIFACT_* below for how it was measured). It
was left behind when the hands were removed from the source, and used as-is it puts a hand on screen that never moves.

The repair is a **rotational median**: the ghost falls inside the concentric metal disc at
r < 90, where the texture is concentric rings, so sampling the same radius at a different
angle gives the right content. For each pixel to repair, samples are taken at the same
radius and several different angles and the median is used -- a median rather than a mean,
so the few samples still landing on the ghost do not affect the result.

Not PIL's inpaint or a Gaussian blur: those flatten the concentric rings and fill in a blur
that is more conspicuous on a metallic dial than the ghost itself.

### 2. Centre on the "knob" rather than the "outer circle"

`png_to_bin.center_dial()` finds the centre by fitting the outer circle, and on this image
that result is **5.6 px** from the knob (outer (221.7, 221.0) vs knob (226.4, 218.0)). The
hands rotate about the asset's centre (see face_render() in render_task.c), so the knob is
what has to be aligned -- a few px on the outer circle is invisible, while the hands
meeting somewhere other than the knob is obvious.

WF4BG.BIN is therefore **not** in png_to_bin.py's AUTO_CENTER; the centring is done in this
tool and the result frozen into src/generated/face4_dial_454_centered.png. Pick that one
when using "Add image / asset" in build_gui.py; picking a source image raises no error but
is visible on screen (the same trap as face3, see the TARGETS comment in png_to_bin.py).

## Three places to keep in sync after a change

    src/asset_store.h    ASSET_FACE4_*_W / _H
    src/render_task.c    PIVOT_F4_* (stored negated)
    tools/png_to_bin.py  TARGETS

A length mismatch makes the firmware treat it as a short read and that hand is not drawn at all.
"""
import os
import sys
import math

try:
    from PIL import Image
except ImportError:
    print("Pillow is required: pip install pillow")
    sys.exit(1)

import png_to_bin

HERE = os.path.dirname(os.path.abspath(__file__))
GEN = os.path.join(HERE, "..", "src", "generated")
SRC_DIR = os.path.join(GEN, "Ambiq_Watch_face")

DIAL_SRC = "Ambiq_Apollo510B_dial_454x454.png"
REF_SRC = "Ambiq_Apollo510B_WatchFace_454x454.png"

#
# The centred and repaired dial, saved back into src/generated. See point 2 in the header.
#
DIAL_CENTERED = "face4_dial_454_centered.png"
DIAL_TARGET = "WF4BG.BIN"

#
# (source file, target, pivot x, pivot y)
#
# How the pivots were measured: the hubs of the hour and minute hands are rings with a
# transparent hole, and the transparent region enclosed by opaque pixels and not connected
# to the image border has a centroid that is the pivot. The measurements are
#   hour    (14.0, 109.0)   hole 37 px
#   minute  (14.5, 172.5)   hole 44 px
#
# x takes the canvas centre (15) rather than the measured 14.0 / 14.5: the hands are
# left-right symmetric and prepare_hand() in split_ambiq_watchface_assets.py centres them
# in the canvas, so the axis is the canvas's vertical midline. The hole is only 37 pixels,
# and a 1 px error in its centroid is less reliable than that reasoning.
#
# The second hand has no ring, so its pivot is the bottom of the canvas (125): it is a
# constant-width rod with no shaping at the tail, and the tail end is the pivot. tail = 0,
# and the end is covered exactly by the knob (the cyan ring is radius 13 and the silver cap about 16).
#
HANDS = [
    ("Ambiq_Apollo510B_hour_30x132.png",   "WF4HR.BIN",  "face4_hour",   15, 109),
    ("Ambiq_Apollo510B_minute_30x205.png", "WF4MIN.BIN", "face4_minute", 15, 172),
    ("Ambiq_Apollo510B_second_18x126.png", "WF4SEC.BIN", "face4_second",  9, 125),
]

#
# The maximum tail length to keep (px).
#
# The source assets' tails are longer than the design: an isolated render gives the **whole**
# hand, while in the design the section near the pivot is hidden by the centre cap. Taking
# brightness along the tails of the reference image, the silver of both the hour and the
# minute hand ends at r = 21 (r = 22 is already the dark ring outside the cap), while a 1:1
# blit of the assets gives
#
#   hour    132 - 109 = 23   only 2 over, invisible
#   minute  205 - 172 = 33   12 over, sticking out past the cap and obviously unlike the design
#   second  126 - 125 =  1   covered anyway
#
# So the tails are cropped to 21 (= CAP_R). What is cropped is always covered by the cap on
# screen, so nothing is lost visually, with a little SSRAM saved along the way.
#
# "Scale the whole hand proportionally" is not used here (that is how face3 keeps its tips
# clear): it would shorten the tips too, and the tip lengths measure correctly as they are
# (design 106/165/120 against assets 109/172/125, 3 to 7 px apart). Only the tail end needs fixing.
#
TAIL_MAX = 21

#
# The centre cap.
#
# This is one asset none of the first three faces needs, and the reason is in the design: the
# cap in the reference composite is **painted above the hands** (the prepare_dial() comment
# in split_ambiq_watchface_assets.py says so too) and its purpose is to cover the hubs of
# all three. The firmware draws dial then hands, so a cap painted on the dial is simply
# covered by the hands.
#
# Leaving it out costs more than "one missing decoration": the hour and minute hubs are
# rings with a short tail below them, and three stacked turn into a cross at the centre
# that also buries the cyan ring. Side by side with the reference they are two different things.
#
# So it is cut out as a separate asset and blitted by face_render() after the three hands.
# psCap is NULL for the first three faces and their behaviour is unchanged (their tails each carry a counterweight disc that stacks into a hub).
#
# The radius of 21 was measured: taking brightness along 16 directions, the median
# brightness at r <= 20 is above 200, while r = 21 drops to 129 and r = 22 to 12 -- the cap's outer edge is right there.
#
# The source is the reference composite rather than the dial: the cap on the dial was
# touched by the ghost repair (the ghost runs straight through it), while the reference's is
# untouched. The cap is the topmost layer in the reference, so the pixels cut out are clean
# and carry no part of a hand.
#
CAP_SRC = REF_SRC
CAP_R = 21.0
CAP_PX = 46
CAP_TARGET = "WF4CAP.BIN"
CAP_FILE = "face4_cap_46x46.png"

#
# The time used for the preview.
#
# Not a round time like 10:10:30 but the angles measured from the reference composite -- that
# image is the only record of this face's "design intent", and the preview is only useful if
# it can be compared against it side by side. The measured axis angles are 309.8 degrees for
# the hour hand, 50.3 for the minute and 179.0 for the second (0 degrees = 12 o'clock, clockwise).
#
# Incidentally, the hands in that image are not horologically consistent: the minute hand is
# at 8.4 minutes while the hour hand is at the 10:20 position (11 minutes apart). Design
# renders often are; there is no need to copy the error -- only its angles are borrowed, to compare lengths.
#
PREVIEW_ANGLES = (309.8, 50.3, 179.0)

#
# Detection range and threshold for the knob's cyan ring.
#
# The cyan ring is the only "central cyan circle" on this dial, so picking it out by colour
# and fitting a least-squares circle is steadier than brightness or a bounding box: the cap
# is silver and the dial has concentric rings, and both interfere with a brightness method.
#
KNOB_WIN = 30
KNOB_R_EXPECT = 13.2

#
# Extent of the hand ghost.
#
#   ARTIFACT_R_IN / OUT   the radius band to repair. The inner edge starts from the knob's
#                         outer edge (the knob itself is real content and must not be
#                         repaired); the outer 92 is the edge of the concentric metal disc,
#                         and beyond it the circuit-board texture begins, which is not
#                         rotationally symmetric and would be filled in wrongly.
#   ARTIFACT_DX           the ghost's horizontal centre (relative to the knob's centre). The
#                         measurement is -3.5: that ghost is not exactly above the knob.
#   ARTIFACT_HALF_W       the half-width. The widest point measures about 16 px (dx -11..+4);
#                         10 is taken to cover the feathered edge as well.
#
ARTIFACT_R_IN = 20.0
ARTIFACT_R_OUT = 92.0
ARTIFACT_DX = -3.5
ARTIFACT_HALF_W = 10.0

#
# Why the inner edge can start at 20: the cap (CAP_R = 21) covers everything at r < 21, so how
# well that region is filled in is invisible. The extra 1 px of overlap keeps the cap's feathered edge from exposing the ghost underneath.
#
# This makes the repair much simpler: the ghost is a fixed 20 px wide, which in angle grows
# wider the smaller the radius (about 60 degrees at r = 20, about 13 at r = 90), while the
# interpolation below is only accurate at narrow angles. The widest section happens to be the invisible one.
#
# The fill is **interpolation along concentric arcs** rather than a rotational median.
#
# The problem with the median (the earlier version) is the vertical sheen gradient on the
# dial: brightest straight up and darker to either side. Samples taken beyond +-35 degrees
# carry darker content, the fill comes out uniformly dark, and a rectangle with visible edges is left behind.
#
# Arc interpolation takes "the pixels immediately either side of the ghost at the same
# radius" and blends linearly along the arc. Both the concentric rings (the contours) and the
# angular gradient survive, and the seam disappears -- the texture in this region is exactly
# concentric, so the interpolation direction follows the contours.
#
# ARC_PAD is how far from the ghost's boundary the samples are taken (degrees): sampling right
# against it would catch the feathering at the edge. ARC_GIVEUP is the threshold for giving up
# and leaving the original when the angle is too wide; in practice it is never reached -- the
# half-width is at most about 30 degrees at r >= 20.
#
ARC_PAD = 3.0
ARC_GIVEUP = 70.0


def knob_center(img):
    """Find the knob's centre by least-squares circle fitting of the cyan ring; returns (cx, cy, r, point count)."""
    rgb = img.convert("RGB")
    w, h = rgb.size
    px = rgb.load()

    cx0, cy0 = w // 2, h // 2
    pts = []

    for y in range(cy0 - KNOB_WIN, cy0 + KNOB_WIN):
        for x in range(cx0 - KNOB_WIN, cx0 + KNOB_WIN):
            r, g, b = px[x, y]
            if (g > 120) and (b > 150) and (r < g - 45):
                pts.append((x, y))

    n = len(pts)
    if n < 30:
        raise ValueError("the knob's cyan ring was not found (only %d points)" % n)

    #
    # Algebraic circle fitting (Kasa). The residual on this image is sub-pixel, so no iterative method is needed.
    #
    sx = sy = sxx = syy = sxy = sxxx = syyy = sxyy = sxxy = 0.0
    for x, y in pts:
        sx += x
        sy += y
        sxx += x * x
        syy += y * y
        sxy += x * y
        sxxx += x ** 3
        syyy += y ** 3
        sxyy += x * y * y
        sxxy += x * x * y

    a = n * sxx - sx * sx
    b = n * sxy - sx * sy
    c = n * syy - sy * sy
    d = 0.5 * (n * sxyy - sx * syy + n * sxxx - sx * sxx)
    e = 0.5 * (n * sxxy - sy * sxx + n * syyy - sy * syy)

    det = a * c - b * b
    kx = (d * c - b * e) / det
    ky = (a * e - b * d) / det
    rr = sum(math.hypot(x - kx, y - ky) for x, y in pts) / n

    return kx, ky, rr, n


def _sample(px, w, h, x, y):
    """Bilinear sampling, clamped at the borders."""
    x = min(max(x, 0.0), w - 1.001)
    y = min(max(y, 0.0), h - 1.001)

    x0, y0 = int(x), int(y)
    fx, fy = x - x0, y - y0

    p00 = px[x0, y0]
    p10 = px[x0 + 1, y0]
    p01 = px[x0, y0 + 1]
    p11 = px[x0 + 1, y0 + 1]

    return tuple(
        int(round(p00[i] * (1 - fx) * (1 - fy) + p10[i] * fx * (1 - fy)
                  + p01[i] * (1 - fx) * fy + p11[i] * fx * fy))
        for i in range(3)
    )


def _bar_edge(kx, ky, r, ang, step):
    """From angle ang, search in direction step for the ghost's boundary angle; None when not found.

    The test is the same condition as the mask (a horizontal band), so the boundary necessarily
    matches the mask -- using one definition for both keeps the filled region from differing from the repaired one by a ring.
    """
    a = ang
    for _ in range(int(ARC_GIVEUP)):
        a += math.radians(step)
        dx = r * math.sin(a)
        if abs(dx - ARTIFACT_DX) > ARTIFACT_HALF_W:
            return a + math.radians(step * ARC_PAD)

    return None


def repair_artifact(img, kx, ky):
    """Interpolate along concentric arcs to repair the vertical hand ghost at the centre. Returns (image, pixels repaired)."""
    rgb = img.convert("RGB")
    w, h = rgb.size
    src = rgb.load()
    out = rgb.copy()
    dst = out.load()

    fixed = 0

    for y in range(h):
        dy = y - ky
        if abs(dy) > ARTIFACT_R_OUT:
            continue

        for x in range(w):
            dx = x - kx

            if abs(dx - ARTIFACT_DX) > ARTIFACT_HALF_W:
                continue

            r = math.hypot(dx, dy)
            if (r < ARTIFACT_R_IN) or (r > ARTIFACT_R_OUT):
                continue

            ang = math.atan2(dx, -dy)

            a_p = _bar_edge(kx, ky, r, ang, +1.0)
            a_m = _bar_edge(kx, ky, r, ang, -1.0)

            if (a_p is None) or (a_m is None):
                continue

            c_p = _sample(src, w, h, kx + r * math.sin(a_p),
                          ky - r * math.cos(a_p))
            c_m = _sample(src, w, h, kx + r * math.sin(a_m),
                          ky - r * math.cos(a_m))

            #
            # Linear blending along the arc. The weight uses the angle difference rather than
            # pixel distance: at one radius, arc length is proportional to angle, so the two
            # are equivalent and the angle needs no multiplication by r.
            #
            t = (ang - a_m) / (a_p - a_m)
            t = min(max(t, 0.0), 1.0)

            dst[x, y] = tuple(
                int(round(c_m[i] * (1.0 - t) + c_p[i] * t)) for i in range(3)
            )
            fixed += 1

    return out, fixed


def prepare_dial(quiet=False):
    """Repair the ghost and centre on the knob; returns the processed dial."""
    img = Image.open(os.path.join(SRC_DIR, DIAL_SRC)).convert("RGB")

    kx, ky, kr, kn = knob_center(img)

    if not quiet:
        bx, by = png_to_bin.dial_center(img)
        print("  knob centre (%.2f, %.2f) radius %.1f (cyan ring %d points)" % (kx, ky, kr, kn))
        print("  outer centre (%.2f, %.2f)  -> %.1f px apart; the knob wins"
              % (bx, by, math.hypot(kx - bx, ky - by)))

    img, fixed = repair_artifact(img, kx, ky)

    if not quiet:
        print("  hand ghost repaired, %d pixels (r %.0f..%.0f, dx %.1f+-%.0f)"
              % (fixed, ARTIFACT_R_IN, ARTIFACT_R_OUT,
                 ARTIFACT_DX, ARTIFACT_HALF_W))

    #
    # Centred on the knob. An integer shift rather than sub-pixel resampling: this dial has
    # very fine concentric rings and circuit traces, resampling would soften them all, and a 0.5 px residual offset is invisible.
    #
    w, h = img.size
    dx = int(round(w / 2.0 - kx))
    dy = int(round(h / 2.0 - ky))

    out = Image.new("RGB", (w, h), (0, 0, 0))
    out.paste(img, (dx, dy))

    if not quiet:
        print("  centring shift (%+d, %+d)" % (dx, dy))

    return out


def hand_image(name, pivot_y):
    """Read one hand and crop an over-long tail. Returns (image, px cropped)."""
    img = Image.open(os.path.join(SRC_DIR, name)).convert("RGBA")
    w, h = img.size

    keep = pivot_y + TAIL_MAX
    if h <= keep:
        return img, 0

    return img.crop((0, 0, w, keep)), h - keep


def prepare_cap(quiet=False):
    """Cut the centre cap out of the reference composite; returns a CAP_PX square RGBA asset."""
    ref = Image.open(os.path.join(SRC_DIR, CAP_SRC)).convert("RGB")
    kx, ky, kr, _ = knob_center(ref)

    #
    # Reconstructed by bilinear sampling about the knob's centre, so sub-pixel offsets are not
    # discarded -- the cap is only 21 px in radius, and half a pixel of offset makes the cyan ring thicker on one side than the other on screen.
    #
    src = ref.load()
    w, h = ref.size

    out = Image.new("RGBA", (CAP_PX, CAP_PX), (0, 0, 0, 0))
    dst = out.load()

    c = (CAP_PX - 1) / 2.0

    for y in range(CAP_PX):
        for x in range(CAP_PX):
            dx = x - c
            dy = y - c
            r = math.hypot(dx, dy)

            #
            # One pixel of feathering at the edge: the cap is round and the asset is square, and
            # a hard edge gives jagged pixels on a rotating screen (the cap itself does not
            # rotate, but it sits over hands that do, at high contrast).
            #
            if r > CAP_R:
                continue

            a = 255 if r <= (CAP_R - 1.0) else int(round(255.0 * (CAP_R - r)))
            if a <= 0:
                continue

            rgb = _sample(src, w, h, kx + dx, ky + dy)
            dst[x, y] = (rgb[0], rgb[1], rgb[2], a)

    if not quiet:
        print("  cap %dx%d  source knob (%.2f, %.2f)  radius %.1f (cyan ring %.1f)"
              % (CAP_PX, CAP_PX, kx, ky, CAP_R, kr))

    return out


def preview(out_path):
    """The dial plus the three hands at the angles measured from the reference, to confirm the pivots and lengths."""
    dial = prepare_dial()

    bg = dial.convert("RGBA")
    w, h = bg.size
    cx, cy = w / 2.0, h / 2.0

    for (name, target, stem, px, py), angle in zip(HANDS, PREVIEW_ANGLES):
        hand, cut = hand_image(name, py)
        hw, hh = hand.size

        layer = Image.new("RGBA", (w, h), (0, 0, 0, 0))
        layer.paste(hand, (int(round(cx - px)), int(round(cy - py))), hand)
        layer = layer.rotate(-angle, resample=Image.BICUBIC, center=(cx, cy))

        bg = Image.alpha_composite(bg, layer)

        print("  %-14s %2dx%-3d  pivot (%d, %d)  tail %d  angle %.1f deg%s"
              % (target, hw, hh, px, py, hh - py, angle,
                 ("  <- tail cropped %d px" % cut) if cut else ""))

    #
    # The cap is blitted last, covering the three hubs -- exactly the order face_render() uses in the firmware.
    #
    cap = prepare_cap()
    c = (CAP_PX - 1) / 2.0
    layer = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    layer.paste(cap, (int(round(cx - c)), int(round(cy - c))), cap)
    bg = Image.alpha_composite(bg, layer)

    bg.convert("RGB").save(out_path)
    print("  preview -> %s" % out_path)


def main(argv):
    if "--preview" in argv:
        preview(os.path.join(GEN, "face4_preview.png"))
        return 0

    os.makedirs(png_to_bin.OUT_DIR, exist_ok=True)

    dial = prepare_dial()
    centered = os.path.join(GEN, DIAL_CENTERED)
    dial.save(centered)
    print("  %-26s saved (centred and repaired dial)" % DIAL_CENTERED)

    ok, msg = png_to_bin.convert(centered, DIAL_TARGET)
    if not ok:
        print("  %s: %s" % (DIAL_CENTERED, msg))
        return 1

    for name, target, stem, px, py in HANDS:
        #
        # The cropped hands are likewise saved back into src/generated before conversion, on
        # the same rule as the dial and the cap: coming in through the GUI's "Add image /
        # asset", the right one can be picked. Picking an original under Ambiq_Watch_face/
        # raises no error (a size mismatch is scaled automatically), but the minute hand's
        # tail sticks out past the cap.
        #
        hand, cut = hand_image(name, py)
        hw, hh = hand.size

        derived = "%s_%dx%d.png" % (stem, hw, hh)
        path = os.path.join(GEN, derived)
        hand.save(path)

        ok, msg = png_to_bin.convert(path, target)
        if not ok:
            print("  %s: %s" % (derived, msg))
            return 1

        print("    ^ %-28s pivot (%d, %d)  tail %d%s"
              % (derived, px, py, hh - py,
                 ("  tail cropped %d px" % cut) if cut else ""))

    cap_path = os.path.join(GEN, CAP_FILE)
    prepare_cap().save(cap_path)
    print("  %-26s saved (centre cap)" % CAP_FILE)

    ok, msg = png_to_bin.convert(cap_path, CAP_TARGET)
    if not ok:
        print("  %s: %s" % (CAP_FILE, msg))
        return 1

    print("  converted 5 assets -> %s" % png_to_bin.OUT_DIR)
    print("  the pivots must match PIVOT_F4_* in render_task.c (stored negated there)")
    print("  then send them to the board's eMMC with asset_upload.py")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
