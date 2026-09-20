#!/usr/bin/env python3
"""Second opinion on rasterisation modes: does the mode break glyphs?

    python3 tools/raster_fidelity.py

raster_lab.py measures stroke evenness, and a high alpha threshold can score
well on it for the wrong reason: if the threshold is high enough, thin strokes
simply vanish, and what is left is very evenly weighted because it is all that
survived. Evenness alone would happily recommend a mode that erases the
crossbar of an e.

So this script measures damage instead:

  * empty     - a printable codepoint that rasterised to no ink at all
  * shattered - a glyph whose 8-connected component count is higher than the
                same glyph rendered with the hinted monochrome rasteriser,
                which means a stroke broke into pieces
  * ink       - total ink relative to monochrome, so thinning is visible

A mode is only a candidate if it damages nothing. Among the undamaged ones,
evenness decides.

Read-only; prints a table and writes tools/raster-lab-out/fidelity.json.
"""

from __future__ import annotations

import json
import pathlib
import sys

from PIL import Image, ImageDraw, ImageFont

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "tools" / "raster-lab-out"

sys.path.insert(0, str(ROOT / "tools"))
from raster_lab import FACES, LEGACY, MODES, SIZES  # noqa: E402

# Latin-1 printable plus the ellipsis, which is what the atlas will carry.
CODEPOINTS = [c for c in list(range(33, 127)) + list(range(161, 256)) + [0x2026]]


def glyph_bitmap(path: pathlib.Path, size: int, mode: str, char: str) -> Image.Image:
    font = ImageFont.truetype(str(path), size)
    pad = size * 2 + 16
    if mode == "mono":
        canvas = Image.new("1", (pad * 3, pad * 3), 0)
        ImageDraw.Draw(canvas).text((pad, pad), char, font=font, fill=1)
        return canvas
    threshold = int(mode.split("-")[1])
    canvas = Image.new("L", (pad * 3, pad * 3), 0)
    ImageDraw.Draw(canvas).text((pad, pad), char, font=font, fill=255)
    return canvas.point(lambda v: 255 if v >= threshold else 0).convert("1")


def components(image: Image.Image) -> int:
    """8-connected components of ink. A broken stroke shows up as an extra one."""
    box = image.getbbox()
    if box is None:
        return 0
    crop = image.crop(box)
    w, h = crop.size
    pixels = crop.load()
    seen = [[False] * w for _ in range(h)]
    count = 0
    for sy in range(h):
        for sx in range(w):
            if not pixels[sx, sy] or seen[sy][sx]:
                continue
            count += 1
            stack = [(sx, sy)]
            seen[sy][sx] = True
            while stack:
                x, y = stack.pop()
                for dy in (-1, 0, 1):
                    for dx in (-1, 0, 1):
                        nx, ny = x + dx, y + dy
                        if 0 <= nx < w and 0 <= ny < h and not seen[ny][nx] and pixels[nx, ny]:
                            seen[ny][nx] = True
                            stack.append((nx, ny))
    return count


def ink(image: Image.Image) -> int:
    return sum(1 for v in image.convert("L").getdata() if v)


def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    report: dict[str, dict] = {}

    print(f"{'face':<20}{'size':>5} {'mode':<8}{'empty':>7}{'shattered':>11}"
          f"{'ink vs mono':>13}")
    for family, weights in {**FACES, **LEGACY}.items():
        for weight, path in weights.items():
            if not path.exists():
                continue
            for size in SIZES:
                baseline_components: dict[str, int] = {}
                baseline_ink = 0
                for codepoint in CODEPOINTS:
                    char = chr(codepoint)
                    image = glyph_bitmap(path, size, "mono", char)
                    baseline_components[char] = components(image)
                    baseline_ink += ink(image)

                for mode in MODES:
                    empty: list[str] = []
                    shattered: list[str] = []
                    total_ink = 0
                    for codepoint in CODEPOINTS:
                        char = chr(codepoint)
                        image = glyph_bitmap(path, size, mode, char)
                        pieces = components(image)
                        total_ink += ink(image)
                        if pieces == 0:
                            empty.append(char)
                        elif pieces > baseline_components[char]:
                            shattered.append(char)
                    key = f"{family}/{weight}/{size}/{mode}"
                    ratio = total_ink / baseline_ink if baseline_ink else 0.0
                    report[key] = {
                        "empty": empty,
                        "shattered": shattered,
                        "ink_ratio": round(ratio, 4),
                    }
                    print(
                        f"{family + '/' + weight:<20}{size:>5} {mode:<8}"
                        f"{len(empty):>7}{len(shattered):>11}{ratio:>13.3f}"
                    )

    (OUT / "fidelity.json").write_text(json.dumps(report, indent=2, sort_keys=True))
    print(f"\nWrote {OUT}/fidelity.json")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
