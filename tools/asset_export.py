# -*- coding: utf-8 -*-
"""
Export the firmware's graphics assets into the .BIN files that go onto eMMC.

The assets have moved to eMMC (see docs/map_navigation/MAP_NAVIGATION_PAGE.md) and the
firmware only reads them. This tool produces exactly the files to copy over -- plug the
board into USB, eMMC mounts as a removable drive, and the whole assets directory goes into its root.

    python tools/asset_export.py
    -> tools/assets_out/HOME.BIN
                       /WATCHBG.BIN
                       /HANDHR.BIN  HANDMIN.BIN  HANDSEC.BIN
                       /MAPTILE0.BIN MAPTILE1.BIN MAPTILE2.BIN

The source is deliberately the firmware's existing C array headers rather than the
original PNGs: those headers are what the hardware currently draws, and re-converting
from PNG could shift the colours through different conversion settings. To regenerate
map tiles from PNG, use map_tile_gen.py, whose output is this tool's input.

The names and sizes have to match ASSET_* in src/asset_store.h; a mismatch makes the
firmware print "short read" at boot.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.dirname(HERE)
SRC = os.path.join(PROJ, "src")
OUT_DIR = os.path.join(HERE, "assets_out")

#
# (output name, source header, array symbol, width, height, bytes per pixel)
#
ASSETS = [
    ("HOME.BIN",     "Generated_2/test_image_rgb565.h",
     "g_ui8TestImageRGB565", 454, 454, 2),
    ("WATCHBG.BIN",  "generated/Watch_face/Watch7Background_noglow_rgb565.h",
     "Watch7Background_noglow_rgb565", 454, 454, 2),
    ("HANDHR.BIN",   "generated/Watch_face/Watch7Hour_rgba.h",
     "Watch7Hour_rgba", 15, 90, 4),
    ("HANDMIN.BIN",  "generated/Watch_face/Watch7Minute_rgba.h",
     "Watch7Minute_rgba", 15, 150, 4),
    ("HANDSEC.BIN",  "generated/Watch_face/Watch7Second_rgba.h",
     "Watch7Second_rgba", 13, 179, 4),
    ("MAPTILE0.BIN", "generated/map/map_tile_00.h", "map_tile_00", 468, 468, 2),
    ("MAPTILE1.BIN", "generated/map/map_tile_01.h", "map_tile_01", 468, 468, 2),
    ("MAPTILE2.BIN", "generated/map/map_tile_02.h", "map_tile_02", 468, 468, 2),
]

BYTE_RE = re.compile(r"0[xX]([0-9a-fA-F]{1,2})")


def extract(path, symbol):
    """Extract the bytes of the named array from a C header."""
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        text = f.read()

    #
    # Find the first '{' after "<symbol>[" and match to its closing '}'. Bracket matching
    # rather than a greedy match, so a file with several arrays is not swallowed whole.
    #
    # There may be attributes between the declaration and the '=', for instance
    #   static const uint8_t g_ui8TestImageRGB565[SIZE] __attribute__((aligned(8))) =
    # so the middle is loosened to "anything except ; and {".
    m = re.search(re.escape(symbol) + r"\s*\[[^\]]*\]\s*[^;{]*=\s*\{", text)
    if not m:
        raise SystemExit("array not found in %s: %s" % (path, symbol))

    start = m.end()
    depth = 1
    i = start
    while i < len(text) and depth > 0:
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
        i += 1

    body = text[start:i - 1]
    return bytes(int(h, 16) for h in BYTE_RE.findall(body))


def main():
    os.makedirs(OUT_DIR, exist_ok=True)

    print("exporting assets -> %s" % OUT_DIR)
    total = 0
    failed = 0

    for name, rel, symbol, w, h, bpp in ASSETS:
        path = os.path.join(SRC, rel)
        if not os.path.isfile(path):
            print("  %-14s missing source %s" % (name, rel))
            failed += 1
            continue

        data = extract(path, symbol)
        expect = w * h * bpp

        if len(data) != expect:
            print("  %-14s size mismatch: %d, expected %d (%dx%d x%dB)"
                  % (name, len(data), expect, w, h, bpp))
            failed += 1
            continue

        with open(os.path.join(OUT_DIR, name), "wb") as f:
            f.write(data)

        print("  %-14s %8d bytes  (%dx%d)" % (name, len(data), w, h))
        total += len(data)

    print("  %d files, %d bytes (%.2f MB)"
          % (len(ASSETS) - failed, total, total / 1024.0 / 1024.0))

    if failed:
        print("  %d assets were not exported" % failed)
        return 1

    print()
    print("Copy the files under %s into the root of the eMMC." % OUT_DIR)
    return 0


if __name__ == "__main__":
    sys.exit(main())
