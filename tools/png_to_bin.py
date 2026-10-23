# -*- coding: utf-8 -*-
"""
Convert PNGs into the asset files (.BIN) that go onto eMMC.

This is the last piece of "swap artwork without changing code":

    PNG --(this tool)--> .BIN --(asset_upload.py)--> eMMC --> screen

Usage:

    python tools/png_to_bin.py my_home.png HOME.BIN
    python tools/png_to_bin.py --dir my_images/      # match by file name
    python tools/png_to_bin.py --targets             # list every target and its spec

The output always goes to tools/assets_out/, the same as asset_export.py, so the GUI's
"Add assets_out" button can pick them up directly.

## Format

An asset file holds **raw pixel data with no header**, so the dimensions and the format
must match exactly what the firmware expects (ASSET_* in src/asset_store.h). The byte
order of both was confirmed by working back from the on-target assets:

  - RGB565: 16-bit little-endian, R in the high 5 bits
  - RGBA8888: the byte order is literally R, G, B, A

On a size mismatch this tool scales automatically and says so -- scaling beats writing a
file of the wrong length, which the firmware treats as a short read and does not draw at all.

## A note on replacing the map

Replacing the map tiles only replaces the base image you see. The navigation route's
coordinates are in src/map_route.h and are a separate matter -- change the base map
without the route and the vehicle drives in the wrong place. See docs/map_navigation/MAP_NAVIGATION_PAGE.md.
"""
import os
import sys

try:
    from PIL import Image
except ImportError:
    print("Pillow is required: pip install pillow")
    sys.exit(1)

HERE = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.join(HERE, "assets_out")

#
# target name -> (width, height, format)
#
# Must match ASSET_*_W / ASSET_*_H in src/asset_store.h. Change one and change the
# other; a mismatch shows up as the firmware printing short read at boot with that asset not drawn at all.
#
TARGETS = {
    "HOME.BIN":     (454, 454, "RGB565"),
    "WATCHBG.BIN":  (454, 454, "RGB565"),
    "HANDHR.BIN":   (15,  90,  "RGBA"),
    "HANDMIN.BIN":  (15,  150, "RGBA"),
    "HANDSEC.BIN":  (13,  179, "RGBA"),
    #
    # The second watch face (0.png and 1..3.rgba.h in src/generated). The three hands
    # come from C headers rather than PNGs, so the whole set goes through
    # tools/face2_prep.py; only the dial can be converted here.
    #
    "WF2BG.BIN":    (390, 390, "RGB565"),
    "WF2HR.BIN":    (22,  104, "RGBA"),
    "WF2MIN.BIN":   (28,  153, "RGBA"),
    "WF2SEC.BIN":   (18,  219, "RGBA"),
    #
    # The third watch face. The dial's source is
    # src/generated/face3_dial_454_centered.png -- **not** either of the two originals under Watch_face/:
    #
    #   face2_3D_dial_454x454.png      the dial's centre is offset (5, 10) up and left, so a
    #                                  direct conversion puts the hands' meeting point outside the knob
    #   face2_3D_numbers_454x454.png   a finished preview (hands already painted on); used as an
    #                                  asset it gives two sets of hands, one of which never moves
    #
    # The centred version is produced by tools/face3_prep.py. Pick that one when using the
    # GUI's "Add image / asset" -- picking an original raises no error here but is visible on screen.
    #
    "WF3BG.BIN":    (454, 454, "RGB565"),
    "WF3HR.BIN":    (23,  107, "RGBA"),
    "WF3MIN.BIN":   (33,  178, "RGBA"),
    "WF3SEC.BIN":   (18,  213, "RGBA"),
    #
    # The fourth watch face (Ambiq Apollo510B). The whole set goes through tools/face4_prep.py.
    #
    # The dial's source is src/generated/face4_dial_454_centered.png -- **not** either of
    # the two under Ambiq_Watch_face/:
    #
    #   Ambiq_Apollo510B_dial_454x454.png    a vertical hand ghost in the middle (the hands
    #                                        were not cleaned off properly), and the outer
    #                                        circle's centre is 5.6 px from the knob; centring must follow the knob
    #   Ambiq_Apollo510B_WatchFace_454x454.png  a finished design reference (hands already
    #                                        painted on); used as an asset it gives two sets of hands
    #
    # The repair and the centring are both done by face4_prep.py, so WF4BG.BIN is
    # **deliberately not in AUTO_CENTER**: that path fits the outer circle, and running it
    # again would push the already-aligned knob 5.6 px off.
    # Pick the centred version when using the GUI's "Add image / asset"; the wrong one raises no error but is visible on screen.
    #
    "WF4BG.BIN":    (454, 454, "RGB565"),
    "WF4HR.BIN":    (30,  130, "RGBA"),
    "WF4MIN.BIN":   (30,  193, "RGBA"),
    "WF4SEC.BIN":   (18,  126, "RGBA"),
    "WF4CAP.BIN":   (46,  46,  "RGBA"),
    "MAPTILE0.BIN": (468, 468, "RGB565"),
    "MAPTILE1.BIN": (468, 468, "RGB565"),
    "MAPTILE2.BIN": (468, 468, "RGB565"),
}


#
# These targets are "round dials", and the dial's centre is aligned to the asset's centre automatically before conversion.
#
# Why do it automatically: the hands rotate about the asset's centre (see face_render() in
# render_task.c), and once the dial's centre is off that point all three hands shift away
# from the dial as a group -- very visible on hardware, with the hands meeting somewhere
# other than the central knob.
#
# And it is easy to bypass: the "Add image / asset" button in build_gui.py calls this
# converter directly, so if centring lived only in some higher-level tool, images coming
# in through the GUI would not be centred and nothing would show it -- the name and the size are identical.
#
# For an already-centred asset it is a no-op (the offset computes to 0) and harmless.
#
AUTO_CENTER = frozenset((
    "WATCHBG.BIN",
    "WF2BG.BIN",
    "WF3BG.BIN",
))


def dial_center(img, thr=40):
    """Find the dial's centre by least-squares circle fitting.

    Why not a bounding box: the dial's edge may run past the asset and be cropped (the top
    of face2_3D_dial loses about 2 px), so the bbox measures a height shorter than the
    width and pulls the centre off. A circle fit is accurate given enough arc and is
    unaffected by a cropped section -- the measured median residual is 0.6 px.

    The boundary points are the leftmost and rightmost of each row and the topmost and
    bottommost of each column, excluding points against the asset's edge (those are exactly
    where the cropping happened and are not on the real circle).
    """
    rgb = img.convert("RGB")
    w, h = rgb.size
    px = rgb.load()

    lum = [[sum(px[x, y]) for x in range(w)] for y in range(h)]
    pts = []

    for y in range(h):
        xs = [x for x in range(w) if lum[y][x] > thr]
        if not xs:
            continue
        for x in (xs[0], xs[-1]):
            if 0 < x < w - 1 and 0 < y < h - 1:
                pts.append((x, y))

    for x in range(w):
        ys = [y for y in range(h) if lum[y][x] > thr]
        if not ys:
            continue
        for y in (ys[0], ys[-1]):
            if 0 < x < w - 1 and 0 < y < h - 1:
                pts.append((x, y))

    if len(pts) < 16:
        return (w - 1) / 2.0, (h - 1) / 2.0

    #
    # The least-squares solution of x^2 + y^2 + Dx + Ey + F = 0, whose centre is (-D/2, -E/2).
    # The 3x3 normal equations are solved in pure Python, saving a numpy dependency.
    #
    n = len(pts)
    sx = sy = sxx = syy = sxy = sb = sbx = sby = 0.0

    for x, y in pts:
        b = -(x * x + y * y)
        sx += x
        sy += y
        sxx += x * x
        syy += y * y
        sxy += x * y
        sb += b
        sbx += b * x
        sby += b * y

    m = [[sxx, sxy, sx], [sxy, syy, sy], [sx, sy, float(n)]]
    v = [sbx, sby, sb]

    for i in range(3):
        p = max(range(i, 3), key=lambda r: abs(m[r][i]))
        m[i], m[p] = m[p], m[i]
        v[i], v[p] = v[p], v[i]
        if abs(m[i][i]) < 1e-9:
            return (w - 1) / 2.0, (h - 1) / 2.0
        for r in range(i + 1, 3):
            f = m[r][i] / m[i][i]
            for c in range(i, 3):
                m[r][c] -= f * m[i][c]
            v[r] -= f * v[i]

    sol = [0.0] * 3
    for i in (2, 1, 0):
        sol[i] = (v[i] - sum(m[i][c] * sol[c]
                             for c in range(i + 1, 3))) / m[i][i]

    return -sol[0] / 2.0, -sol[1] / 2.0


def center_dial(img):
    """Translate the dial to the exact centre of the asset. Returns (image, dx, dy, original centre).

    The translation is an integer: a sub-pixel shift would resample the whole image, and blurring the picture for 0.1 px is not worth it.
    """
    w, h = img.size
    cx, cy = dial_center(img)

    dx = int(round(((w - 1) / 2.0) - cx))
    dy = int(round(((h - 1) / 2.0) - cy))

    if (dx == 0) and (dy == 0):
        return img, 0, 0, (cx, cy)

    out = Image.new(img.mode, (w, h), 0)
    out.paste(img, (dx, dy))

    return out, dx, dy, (cx, cy)


def to_rgb565(img):
    """RGB565, little-endian."""
    px = img.convert("RGB").load()
    w, h = img.size
    out = bytearray(w * h * 2)
    i = 0
    for y in range(h):
        for x in range(w):
            r, g, b = px[x, y]
            v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
            out[i] = v & 0xFF
            out[i + 1] = v >> 8
            i += 2
    return bytes(out)


def to_rgba(img):
    """RGBA8888, byte order R, G, B, A.

    The order was worked back from the on-target assets: the stroke pixels of the hour hand
    (HANDHR.BIN) are (255, 0, 0, 255), and that hand is red on screen.
    """
    return img.convert("RGBA").tobytes()


def convert(src, target, quiet=False):
    """Convert one PNG into the given target asset. Returns (ok, message)."""
    target = target.upper()
    if target not in TARGETS:
        return False, "unknown target: %s (use --targets for the list)" % target

    w, h, fmt = TARGETS[target]

    try:
        img = Image.open(src)
    except Exception as e:
        return False, "cannot read %s: %s" % (src, e)

    note = ""
    if img.size != (w, h):
        #
        # Automatic scaling, with LANCZOS: the assets are mostly maps or dials with fine
        # lines, and nearest neighbour would drop every other tick and route line.
        #
        note = " (scaled from %dx%d)" % img.size
        img = img.resize((w, h), Image.LANCZOS)

    #
    # Dial targets are centred automatically. After the scaling: the centre has to align to the final size's centre.
    #
    if target in AUTO_CENTER:
        img, dx, dy, (ocx, ocy) = center_dial(img)
        if (dx != 0) or (dy != 0):
            note += " (dial centre %.1f,%.1f -> shifted %+d,%+d to centre)" % (ocx, ocy,
                                                                    dx, dy)

    data = to_rgb565(img) if fmt == "RGB565" else to_rgba(img)

    expect = w * h * (2 if fmt == "RGB565" else 4)
    if len(data) != expect:
        return False, "internal error: produced %d bytes, expected %d" % (len(data), expect)

    os.makedirs(OUT_DIR, exist_ok=True)
    out = os.path.join(OUT_DIR, target)
    with open(out, "wb") as f:
        f.write(data)

    if not quiet:
        print("  %-14s %8d bytes  %dx%d %s%s"
              % (target, len(data), w, h, fmt, note))

    return True, out


def convert_dir(path):
    """Match the PNGs in a folder to targets by file name.

    The rule is "drop the extension and append .BIN", e.g. HOME.png -> HOME.BIN.
    Names like HOME.BIN.png are accepted too.
    """
    if not os.path.isdir(path):
        print("not a folder: %s" % path)
        return 1

    done = 0
    skipped = []

    for name in sorted(os.listdir(path)):
        full = os.path.join(path, name)
        if not os.path.isfile(full):
            continue

        stem = os.path.splitext(name)[0].upper()
        target = stem if stem.endswith(".BIN") else stem + ".BIN"

        if target not in TARGETS:
            skipped.append(name)
            continue

        ok, msg = convert(full, target)
        if ok:
            done += 1
        else:
            print("  %s: %s" % (name, msg))

    print("  converted %d assets -> %s" % (done, OUT_DIR))
    if skipped:
        print("  skipped (file name matches no target): %s" % ", ".join(skipped))
        print("  see --targets for the available target names")
    return 0 if done else 1


def show_targets():
    print("Available target assets (the size and format must match exactly):")
    print()
    print("  %-14s %-12s %-9s %s" % ("target", "size", "format", "bytes"))
    for name, (w, h, fmt) in TARGETS.items():
        size = w * h * (2 if fmt == "RGB565" else 4)
        print("  %-14s %-12s %-9s %d" % (name, "%dx%d" % (w, h), fmt, size))
    print()
    print("A size mismatch is scaled automatically. The format follows the target and need not be given.")


def main():
    args = sys.argv[1:]

    if not args or "--help" in args or "-h" in args:
        print(__doc__)
        return 0

    if "--targets" in args:
        show_targets()
        return 0

    if "--dir" in args:
        i = args.index("--dir")
        if i + 1 >= len(args):
            print("--dir must be followed by a folder")
            return 1
        print("PNG -> asset files")
        return convert_dir(args[i + 1])

    if len(args) != 2:
        print("usage: python tools/png_to_bin.py <image> <target name>")
        print("       python tools/png_to_bin.py --dir <folder>")
        print("      python tools/png_to_bin.py --targets")
        return 1

    print("PNG -> asset files")
    ok, msg = convert(args[0], args[1])
    if not ok:
        print("  " + msg)
        return 1

    print()
    print("Then send them to eMMC with tools/asset_upload.py, or press \"Add assets_out\" in the GUI.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
