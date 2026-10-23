# -*- coding: utf-8 -*-
"""Convert the second watch face's assets into the .BIN files for eMMC.

    python tools/face2_prep.py
    python tools/face2_prep.py --preview    # compose a preview to check the pivots

## Where the assets come from

    src/generated/0.png        390x390  dial
    src/generated/1.rgba.h     22x104   hour hand
    src/generated/2.rgba.h     28x153   minute hand
    src/generated/3.rgba.h     18x219   second hand

The hands come from C headers rather than PNGs -- this directory has no matching
1.png/2.png/3.png, only the generated arrays. They hold raw RGBA8888 in exactly the same
byte order as a .BIN, so this tool only pulls the 0x?? values out one by one and writes
them to a file, with no conversion at all.

## Do not use w0..w3.png

The same directory also holds a w0..w3.png set of almost identical artwork and similar
sizes (360x360 and 32x144/32x152/32x220), and it is easy to take the wrong one. They are a
**different design**:

  - the hands' pivot is at the centre of the counterweight disc at the tail, while 1..3's pivots fall outside the asset
  - the alpha of w1..w3 is compressed into [128, 255] (128 marks the background) and has to be un-mixed;
    the alpha of 1..3.rgba.h is a normal 0..255

Mixing the two shows up as the hour hand growing out of the knob at the centre of the dial, at the wrong length too.

The set that matches this dial is 0.png + 1..3.rgba.h -- the combination used by the
example of the same name in boards/apollo510dL_evb, whose pivot values are carried over
as well (see PIVOT_F2_* in render_task.c).

## Pivots

    hour    22x104   pivot (11, 140)   the pivot is 36 px below the asset
    minute  28x153   pivot (14, 139)
    second  18x219   pivot (9, 148)    71 px of tail below the pivot

The hour hand's "pivot outside the asset" is not a typo: there is a knob about 70 px
across at the centre of the dial, and all three hands start from its outer edge. The
image produced by --preview shows this directly.
"""
import os
import re
import sys

try:
    from PIL import Image
except ImportError:
    print("Pillow is required: pip install pillow")
    sys.exit(1)

import png_to_bin

HERE = os.path.dirname(os.path.abspath(__file__))
GEN = os.path.join(HERE, "..", "src", "generated")

#
# The dial goes through png_to_bin.py's ordinary path (PNG -> RGB565).
#
DIAL_SRC = "0.png"
DIAL_TARGET = "WF2BG.BIN"

#
# Hands: C header -> target -> (width, height, pivot). The pivot is used only by --preview
# and by the documentation; the firmware's copy is PIVOT_F2_* in render_task.c, and the two have to change together.
#
HANDS = [
    ("1.rgba.h", "WF2HR.BIN",  (22, 104), (11, 140)),
    ("2.rgba.h", "WF2MIN.BIN", (28, 153), (14, 139)),
    ("3.rgba.h", "WF2SEC.BIN", (18, 219), (9,  148)),
]

#
# Where the three hands point in the preview. 10:09:30 is the conventional catalogue-photo angle, with no two hands overlapping.
#
PREVIEW_TIME = (10, 9, 30)


def read_c_array(path):
    """Pull every 0x?? out of a C header and return them as bytes."""
    with open(path, encoding="utf-8", errors="ignore") as f:
        text = f.read()

    #
    # Start from the array definition, or hexadecimal numbers in the header comment (version numbers and so on) would be counted too.
    #
    start = text.index("[] =")

    return bytes(int(v, 16) for v in re.findall(r"0x([0-9a-fA-F]{2})",
                                                text[start:]))


def hand_image(name, size):
    """Restore a hand's C array into a PIL image (for --preview)."""
    data = read_c_array(os.path.join(GEN, name))

    return Image.frombytes("RGBA", size, data)


def preview(out_path):
    """Compose the dial and the three hands into one image, to confirm the pivots are right.

    A wrong pivot shows up on hardware as the hand circling an offset point, but only after flashing.
    Composing once on the PC with the same pivots makes a wrong one visible in this image.

    This is for verification and is not part of the production flow -- the BIN files do not go through here.
    """
    hh, mm, ss = PREVIEW_TIME

    bg = Image.open(os.path.join(GEN, DIAL_SRC)).convert("RGBA")
    w, h = bg.size
    cx, cy = w / 2.0, h / 2.0

    angles = [
        (hh + mm / 60.0) / 12.0 * 360.0,
        mm / 60.0 * 360.0,
        ss / 60.0 * 360.0,
    ]

    for (name, _, size, (px, py)), angle in zip(HANDS, angles):
        hand = hand_image(name, size)

        #
        # The hand is first pasted onto a transparent layer the size of the dial so that
        # its pivot lands exactly on the dial's centre, then the whole layer is rotated
        # about that centre. The firmware's draw_hand() does the same thing, except it
        # computes the four corners directly without this intermediate layer.
        #
        layer = Image.new("RGBA", (w, h), (0, 0, 0, 0))
        layer.paste(hand, (int(round(cx - px)), int(round(cy - py))), hand)
        layer = layer.rotate(-angle, resample=Image.BICUBIC, center=(cx, cy))

        bg = Image.alpha_composite(bg, layer)

    bg.save(out_path)
    print("  preview -> %s (%02d:%02d:%02d)" % ((out_path,) + PREVIEW_TIME))


def export_png():
    """Save the three hands' C arrays as PNGs, for viewing in an image viewer.

    Only the dial has a PNG in this directory (0.png); the three hands are C arrays from
    start to finish -- seeing what they look like used to require finding a tool that
    parses C headers. The saved files are for human eyes only; the production flow does
    not read them (a .BIN is always converted straight from the .h).

    Deliberately not named 1.png / 2.png / 3.png: that would suggest they are the source of
    the .h files, and editing a PNG would affect no output at all.
    """
    for name, target, size, _ in HANDS:
        out = os.path.join(GEN, "face2_" + target[3:].split(".")[0].lower()
                           + ".png")
        hand_image(name, size).save(out)
        print("  %s -> %s  %dx%d" % (name, os.path.basename(out),
                                     size[0], size[1]))

    return 0


def main(argv):
    if "--export-png" in argv:
        return export_png()

    if "--preview" in argv:
        preview(os.path.join(GEN, "face2_preview.png"))
        return 0

    ok, msg = png_to_bin.convert(os.path.join(GEN, DIAL_SRC), DIAL_TARGET)
    if not ok:
        print("  %s: %s" % (DIAL_SRC, msg))
        return 1

    os.makedirs(png_to_bin.OUT_DIR, exist_ok=True)

    for name, target, (w, h), _ in HANDS:
        data = read_c_array(os.path.join(GEN, name))

        expect = w * h * 4
        if len(data) != expect:
            print("  %s: the array has %d bytes, expected %d (size mismatch?)"
                  % (name, len(data), expect))
            return 1

        with open(os.path.join(png_to_bin.OUT_DIR, target), "wb") as f:
            f.write(data)

        print("  %-14s %8d bytes  %dx%d RGBA (from %s)"
              % (target, len(data), w, h, name))

    print("  converted 4 assets -> %s" % png_to_bin.OUT_DIR)
    print("  then send them to the board's eMMC with asset_upload.py")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
