"""Parse text widths from a NemaVG font's .c file, so the canvas preview and the hardware share one set of metrics.

Why this is needed
--------------
The canvas preview used to lay text out with a system font on the PC (Microsoft JhengHei),
while hardware uses the NemaVG font configured for the project. The widths differ at the
same size, so a line that clearly fits on the canvas loses its last letter or two once
flashed -- `nema_vg_print()` simply clips whatever exceeds the width.

The fix is to read the advance table out of the hardware's font and lay the preview out
from it. The glyphs are still the system font's (the outlines are not parsed), but how
wide each character is comes out right, and that is what decides whether it gets clipped.

About converting the font size
--------------
`xAdvance` is in font units, and converting to pixels divides by a basis. That basis is not
defined in the SDK documentation (the vector graphics manual only says the size given to
`nema_vg_set_font_size()` is a pixel height, without saying which font metric it maps to),
and NemaVG ships no source, so it was worked back from on-target measurements:

    Measured on apollo510b_evb with nema_vg_string_get_bbox() on DejaVuSans_ttf_kern

      size=28  "A"     -> w=25.54   (xAdvance of "A" = 1401)
      size=28  "test"  -> w=71.70   (advance total 3933)
      size=28  any string -> h=43.46   (the font's size field = 2384)

    All three independent expressions give the same denominator:

      1401 * 28 / 25.54 = 1536.0
      3933 * 28 / 71.70 = 1536.0
      2384 * 28 / 43.46 = 1535.9

    1536 = units_per_em * 3/4, i.e. the 72/96 relationship between point and pixel at
    96 DPI -- NemaVG treats size as points, 1 pt = 4/3 px. This agrees with the manual,
    where `nema_vg_get_ascender_pt()` returns "point units".

The measurements also settled two things: the bbox width is the **sum of the advances**
rather than the actual ink extent (computed from bbox_xmin/xmax instead, "A" and "test"
go from zero error to 0.6 to 1.9 px off); and kerning is not fully reflected in the bbox
("AAAA" measures between the values with and without kerning), so no kerning is applied here.

Result: strings with no kerning pairs have zero error, and strings with kerning are within
1.6 px (about 0.5%), which is enough for canvas layout.

What really guarantees text is not clipped is still `ui_designer_draw_text()` in the
generated code -- it measures with `nema_vg_string_get_bbox()` on hardware and shrinks the
size automatically, using NemaVG's own metrics and depending on none of the estimation here.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import re


#
# Each row of the glyph array looks like this (the field order is nema_vg_glyph_t in nema_vg_font.h):
#
#   {  data_offset, data_len, seg_offset, seg_len, xAdvance, kern_off, kern_len,
#      bbox_xmin, bbox_ymin, bbox_xmax, bbox_ymax},   // 0x00000041
#
# Only xAdvance (index 4) and the Unicode code point in the trailing comment are needed.
#
_GLYPH_ROW = re.compile(r"\{([^{}]*)\}\s*,\s*//\s*0x([0-9A-Fa-f]+)")
_GLYPH_ARRAY = re.compile(r"nema_vg_glyph_t\s+\w+\[\]\s*=\s*\{")

#
# The nema_vg_font_t initializer block at the end of the file. Every font-level field has to
# be found inside this block and not by searching the whole file -- the glyph and kerning
# arrays hold tens of thousands of numbers, and a blind search hits those first.
#
_FONT_STRUCT = re.compile(r"nema_vg_font_t\s+\w+\s*=\s*\{")

#
# The fields inside the block that carry a comment. The conversion tool's comment format is
# fixed (`651.00f, //xAdvance`), and matching the comment is steadier than counting fields.
#
_FONT_FIELD = re.compile(r"([-\d.]+)f?\s*,\s*//\s*(xAdvance|size|ascender|descender)")

#
# units_per_em is the block's last field and carries no comment, so it is matched as "the last plain decimal integer".
#
_TRAILING_INT = re.compile(r"(\d+)\s*,?\s*$")


#
# The divisor converting font units to pixels = units_per_em * POINT_TO_PIXEL_DIVISOR.
# 1536/2048 = 3/4 = 72/96, i.e. NemaVG takes the size as points and converts at 96 DPI.
# The derivation and the on-target measurements are in the module docstring.
#
_POINT_SCALE = 3.0 / 4.0


@dataclass(frozen=True)
class FontMetrics:
    """The text width data of one NemaVG font."""

    units_per_em: int
    default_advance: float           # fallback for characters not found (the font's xAdvance field)
    advances: dict[int, float]       # code point -> advance, in font units
    source: Path

    def text_width(self, text: str, font_size_px: float) -> float:
        """Compute a line's pixel width at the given size, matching the hardware's bbox measurement.

        Kerning is not counted: measurement shows NemaVG's bbox does not fully reflect it,
        and counting it makes the error worse (see the module docstring).
        """
        if not text or self.units_per_em <= 0:
            return 0.0
        units = sum(self.advances.get(ord(char), self.default_advance) for char in text)
        return units * font_size_px / (float(self.units_per_em) * _POINT_SCALE)


def _find_font_source(font_header: str, search_from: Path) -> Path | None:
    """Search upward from the output directory for the font's .c file.

    font_header in the project settings is the path as written in the include (e.g.
    font/DejaVuSans_ttf_kern.h), relative to some -I directory, and the generator does not
    know which. So each level up from the output directory is tried, with both "appended
    directly" and "appended under src/" -- covering this project's src/font/... layout.

    None is returned when nothing is found: width estimation is a convenience, and Generate C still has to complete without it.
    """
    relative = Path(font_header).with_suffix(".c")
    base = search_from if search_from.is_dir() else search_from.parent
    for level in [base, *base.parents][:6]:
        for candidate in (level / relative, level / "src" / relative):
            if candidate.is_file():
                return candidate
    return None


def load(font_header: str, search_from: Path) -> FontMetrics | None:
    """Read the font's advance table. Any failed step returns None, and the caller skips the estimation."""
    source = _find_font_source(font_header, Path(search_from))
    if source is None:
        return None
    try:
        text = source.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None

    advances: dict[int, float] = {}
    for match in _GLYPH_ARRAY.finditer(text):
        body = text[match.end():]
        end = body.find("};")
        if end < 0:
            continue
        for fields, code in _GLYPH_ROW.findall(body[:end]):
            parts = [part.strip() for part in fields.split(",")]
            if len(parts) < 5:
                continue
            try:
                advances[int(code, 16)] = float(parts[4].rstrip("f"))
            except ValueError:
                continue
    if not advances:
        return None

    struct_match = _FONT_STRUCT.search(text)
    if struct_match is None:
        return None
    body = text[struct_match.end():]
    end = body.find("}")
    if end < 0:
        return None
    body = body[:end]

    named = {name: float(value) for value, name in _FONT_FIELD.findall(body)}

    #
    # The last field is units_per_em. Lines are scanned from the end for the first one
    # ending in an integer, skipping blank and comment lines.
    #
    units_per_em = 0
    for line in reversed(body.splitlines()):
        line = line.split("//")[0].strip()
        if not line:
            continue
        trailing = _TRAILING_INT.search(line)
        if trailing:
            units_per_em = int(trailing.group(1))
        break
    if units_per_em <= 0:
        return None

    return FontMetrics(
        units_per_em=units_per_em,
        default_advance=named.get("xAdvance", 0.0),
        advances=advances,
        source=source,
    )


def missing_glyphs(metrics: FontMetrics, text: str) -> list[str]:
    """List the characters the font does not have, deduplicated and in order of appearance."""
    missing: list[str] = []
    for char in text:
        if ord(char) not in metrics.advances and char not in missing:
            missing.append(char)
    return missing
