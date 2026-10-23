"""Prepare the Ambiq Apollo510B watch-face PNG assets.

The source watch image is flattened.  The four editable inputs passed to this
script are the hand-free dial and the three isolated hand renders.  This step
normalizes them to the compact PNG layout used by the NemaGFX watch-face
example: an RGB 454x454 dial and three tightly packed RGBA hand sprites.
"""

from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter


def resize_rgba_premultiplied(image: Image.Image, size: tuple[int, int]) -> Image.Image:
    """Resize RGBA without introducing dark fringes at translucent edges."""
    rgba = image.convert("RGBA")
    alpha = rgba.getchannel("A")
    premultiplied = Image.new("RGBA", rgba.size)
    premultiplied.putdata(
        [
            (
                (r * a + 127) // 255,
                (g * a + 127) // 255,
                (b * a + 127) // 255,
                a,
            )
            for r, g, b, a in rgba.getdata()
        ]
    )
    resized = premultiplied.resize(size, Image.Resampling.LANCZOS)
    pixels = []
    for r, g, b, a in resized.getdata():
        if a:
            pixels.append(
                (
                    min(255, (r * 255 + a // 2) // a),
                    min(255, (g * 255 + a // 2) // a),
                    min(255, (b * 255 + a // 2) // a),
                    a,
                )
            )
        else:
            pixels.append((0, 0, 0, 0))
    result = Image.new("RGBA", size)
    result.putdata(pixels)
    return result


def prepare_hand(source: Path, canvas_size: tuple[int, int]) -> Image.Image:
    image = Image.open(source).convert("RGBA")
    alpha = image.getchannel("A")

    # Image generation may leave isolated alpha=1..3 pixels far from the hand.
    clean_alpha = alpha.point(lambda value: value if value >= 4 else 0)
    image.putalpha(clean_alpha)
    bbox = clean_alpha.getbbox()
    if bbox is None:
        raise ValueError(f"No visible pixels found in {source}")

    left, top, right, bottom = bbox
    margin = 4
    crop_box = (
        max(0, left - margin),
        max(0, top - margin),
        min(image.width, right + margin),
        min(image.height, bottom + margin),
    )
    trimmed = image.crop(crop_box)

    out_w, out_h = canvas_size
    usable_w = max(1, out_w - 2)
    usable_h = max(1, out_h - 2)
    scale = min(usable_w / trimmed.width, usable_h / trimmed.height)
    scaled_size = (
        max(1, round(trimmed.width * scale)),
        max(1, round(trimmed.height * scale)),
    )
    trimmed = resize_rgba_premultiplied(trimmed, scaled_size)

    output = Image.new("RGBA", canvas_size, (0, 0, 0, 0))
    offset = ((out_w - scaled_size[0]) // 2, (out_h - scaled_size[1]) // 2)
    output.alpha_composite(trimmed, offset)
    return output


def prepare_dial(generated_dial: Path, flattened_source: Path) -> Image.Image:
    dial = Image.open(generated_dial).convert("RGB")
    dial = dial.resize((454, 454), Image.Resampling.LANCZOS)

    # The reference face keeps its polished center cap on the dial so it can
    # cover the rotating hand roots. Restore that cap from the source image.
    original = Image.open(flattened_source).convert("RGB")
    if original.size != (454, 454):
        original = original.resize((454, 454), Image.Resampling.LANCZOS)

    center = (227, 218)
    mask = Image.new("L", dial.size, 0)
    draw = ImageDraw.Draw(mask)
    radius = 16
    draw.ellipse(
        (
            center[0] - radius,
            center[1] - radius,
            center[0] + radius,
            center[1] + radius,
        ),
        fill=255,
    )
    mask = mask.filter(ImageFilter.GaussianBlur(0.75))
    return Image.composite(original, dial, mask)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--dial", required=True, type=Path)
    parser.add_argument("--hour", required=True, type=Path)
    parser.add_argument("--minute", required=True, type=Path)
    parser.add_argument("--second", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)

    assets = {
        "Ambiq_Apollo510B_dial_454x454.png": prepare_dial(args.dial, args.source),
        "Ambiq_Apollo510B_hour_30x132.png": prepare_hand(args.hour, (30, 132)),
        "Ambiq_Apollo510B_minute_30x205.png": prepare_hand(args.minute, (30, 205)),
        # In the flattened design the cyan marker is at the second-hand tip,
        # away from the center axle. Rotate the extracted render so it matches
        # the reference sprites' canonical 12-o'clock direction.
        "Ambiq_Apollo510B_second_18x126.png": prepare_hand(
            args.second, (18, 126)
        ).transpose(Image.Transpose.ROTATE_180),
    }
    for filename, image in assets.items():
        image.save(args.output_dir / filename, optimize=True)


if __name__ == "__main__":
    main()
