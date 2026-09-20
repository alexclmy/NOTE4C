#!/usr/bin/env python3
"""Measure which rasterisation mode gives the most even strokes.

    python3 tools/raster_lab.py

the owner's complaint about the panel was specific: "small raster letters have
visibly uneven weight". That is a measurable property, not a taste, so this
script measures it rather than arguing about it.

The metric is STEM EVENNESS. In the x-height band of a lowercase alphabet,
almost every horizontal run of ink is a vertical stem crossing. On a well
rasterised small face those runs are nearly all the same width: every stem is
1 px, or every stem is 2 px. On a badly rasterised one the same alphabet mixes
1 px and 2 px stems, because an unhinted outline lands on fractional pixel
boundaries and the threshold rounds each stem independently. That mixture is
exactly what reads as "uneven weight" at arm's length.

  evenness = (runs at the modal width) / (all runs), in the x-height band

1.0 means every stem in the alphabet is the same width. The lower it gets, the
lumpier the text looks.

Read-only: writes nothing but PNG contact sheets and a JSON summary into
tools/raster-lab-out/, which is gitignored.
"""

from __future__ import annotations

import collections
import json
import pathlib
import sys

from PIL import Image, ImageDraw, ImageFont

ROOT = pathlib.Path(__file__).resolve().parent.parent
FONTS = ROOT / "assets" / "fonts"
OUT = ROOT / "tools" / "raster-lab-out"

# The lowercase alphabet plus the French accented forms the panel actually
# renders. Digits are included because temperatures are the thing the owner looks at
# from across the room.
SAMPLE = "abcdefghijklmnopqrstuvwxyz0123456789éèêàçôûùïö"

FACES = {
    "inter": {
        "regular": FONTS / "inter" / "Inter_400Regular.ttf",
        "bold": FONTS / "inter" / "Inter_700Bold.ttf",
    },
    "atkinson": {
        "regular": FONTS / "atkinson" / "AtkinsonHyperlegible_400Regular.ttf",
        "bold": FONTS / "atkinson" / "AtkinsonHyperlegible_700Bold.ttf",
    },
    "plexmono": {
        "regular": FONTS / "plexmono" / "IBMPlexMono_400Regular.ttf",
        "bold": FONTS / "plexmono" / "IBMPlexMono_700Bold.ttf",
    },
}

# The Arial faces the tower ships today, kept in the comparison so the report
# can say whether the new pipeline is actually better than what the owner rejected.
LEGACY = {
    "arial": {
        "regular": pathlib.Path("/System/Library/Fonts/Supplemental/Arial.ttf"),
        "bold": pathlib.Path("/System/Library/Fonts/Supplemental/Arial Bold.ttf"),
    }
}

SIZES = [11, 13, 15, 18, 22, 27, 34]

MODES = ["mono", "aa-96", "aa-110", "aa-128", "aa-150"]


def render_sample(path: pathlib.Path, size: int, mode: str) -> Image.Image:
    """One 1-bit image of SAMPLE, rasterised the way `mode` says."""
    font = ImageFont.truetype(str(path), size)
    pad = size * 2
    width = int(font.getlength(SAMPLE)) + pad * 2
    height = size * 4

    if mode == "mono":
        # A "1" target makes FreeType render FT_RENDER_MODE_MONO with the
        # hinter on, so stems are snapped to whole pixels before they are
        # sampled. Nothing downstream has to guess where the stem edge was.
        canvas = Image.new("1", (width, height), 0)
        ImageDraw.Draw(canvas).text((pad, pad), SAMPLE, font=font, fill=1)
        return canvas

    threshold = int(mode.split("-")[1])
    canvas = Image.new("L", (width, height), 0)
    ImageDraw.Draw(canvas).text((pad, pad), SAMPLE, font=font, fill=255)
    return canvas.point(lambda v: 255 if v >= threshold else 0).convert("1")


def stem_runs(image: Image.Image, font: ImageFont.FreeTypeFont, size: int) -> list[int]:
    """Horizontal ink runs inside the x-height band.

    The band is where stems live. Including ascenders and descenders would
    also include round tops and bottoms, whose runs are legitimately wide and
    would drown the signal we are after.
    """
    box = image.getbbox()
    if box is None:
        return []
    ascent, _ = font.getmetrics()
    pad = size * 2
    baseline = pad + ascent
    x_height = font.getbbox("x")
    top = pad + x_height[1]
    bottom = pad + x_height[3]
    # Trim a row at each end so the flat top and bottom of x itself, which are
    # horizontal strokes and not stems, stay out of the sample.
    top += 1
    bottom -= 1
    if bottom <= top:
        return []

    pixels = image.load()
    runs: list[int] = []
    for y in range(max(0, top), min(image.height, bottom)):
        run = 0
        for x in range(image.width):
            if pixels[x, y]:
                run += 1
            elif run:
                runs.append(run)
                run = 0
        if run:
            runs.append(run)
    del baseline
    return runs


def evenness(runs: list[int]) -> tuple[float, int, int]:
    if not runs:
        return (0.0, 0, 0)
    counts = collections.Counter(runs)
    modal_width, modal_count = counts.most_common(1)[0]
    return (modal_count / len(runs), modal_width, len(runs))


def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    all_faces = {**FACES, **LEGACY}

    summary: dict[str, dict] = {}
    sheets: list[tuple[str, Image.Image]] = []

    for family, weights in all_faces.items():
        for weight, path in weights.items():
            if not path.exists():
                print(f"  skip {family}/{weight}: {path} missing", file=sys.stderr)
                continue
            for size in SIZES:
                font = ImageFont.truetype(str(path), size)
                for mode in MODES:
                    image = render_sample(path, size, mode)
                    runs = stem_runs(image, font, size)
                    score, modal, total = evenness(runs)
                    key = f"{family}/{weight}/{size}/{mode}"
                    summary[key] = {
                        "evenness": round(score, 4),
                        "modal_stem_px": modal,
                        "runs": total,
                        "ink": sum(image.point(lambda v: 1 if v else 0)
                                   .convert("L").getdata()),
                    }
                    if size in (11, 13, 15):
                        sheets.append((key, image))
                    print(
                        f"  {key:<40} evenness {score:.3f}  modal stem {modal}px"
                        f"  ({total} runs)",
                        file=sys.stderr,
                    )

    (OUT / "summary.json").write_text(json.dumps(summary, indent=2, sort_keys=True))

    # Contact sheets, 4x scaled, so a human can confirm the number matches the
    # eye before anything is decided on it.
    for key, image in sheets:
        name = key.replace("/", "_") + ".png"
        box = image.getbbox()
        if box is None:
            continue
        crop = image.crop(box).convert("L")
        crop = crop.resize((crop.width * 4, crop.height * 4), Image.NEAREST)
        Image.eval(crop, lambda v: 255 - v).save(OUT / name)

    print(f"\nWrote {OUT}/summary.json and {len(sheets)} contact sheets.",
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
