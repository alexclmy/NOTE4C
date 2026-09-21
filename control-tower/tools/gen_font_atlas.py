#!/usr/bin/env python3
"""Rasterise the tower's glyph atlases from the vendored OFL faces.

Run manually with any Python 3 that has Pillow installed:

    python3 tools/gen_font_atlas.py

Writes one JSON file per family into src/core/render/fonts/ and nothing else.
It reads only assets/fonts/, so regeneration is hermetic: no network, no
system font directory, no other project.

WHY THESE FACES
---------------
The previous version of this file rasterised Arial from
/System/Library/Fonts/Supplemental. Arial is licensed to Apple by Monotype for
use on the machine; committing bitmaps derived from it is redistribution, and
nothing in that licence permits it. The four families here are all SIL Open
Font License 1.1, which permits redistribution of the fonts and of derived
works, so the generated atlases can live in git honestly. Their licence texts
are vendored next to them in assets/fonts/<family>/OFL.txt.

WHY MONOCHROME HINTING
----------------------
The old pipeline rendered anti-aliased and kept every pixel whose alpha was
>= 110. That threshold decides each stem's width independently of every other
stem, so an alphabet comes out with some stems 1 px and some 2 px, which is
the "visibly uneven weight" the owner rejected on the physical panel.

This pipeline asks FreeType for FT_RENDER_MODE_MONO instead, which runs the
hinter and grid-fits the outline before sampling, so stems are snapped to
whole pixels as a set. tools/raster_lab.py measures the difference and
tools/raster_fidelity.py checks the cost: monochrome is the only mode of the
five tested that breaks no glyph at any size, and it has the best stroke
evenness of the modes that break nothing.

Advances are taken in the same mode, so they are whole pixels too. Fractional
advances would let the pen drift off the pixel grid between letters and
reintroduce the unevenness the hinting just removed.

NO SYNTHETIC BOLD
-----------------
Every weight here is a real face drawn by the type designer. Emboldening a
regular face by smearing it a pixel is what produces lumpy stems, so the
generator has no such path and the renderer has no way to ask for one.
"""

from __future__ import annotations

import base64
import hashlib
import json
import os
import pathlib
import sys

from PIL import Image, ImageDraw, ImageFont

ROOT = pathlib.Path(__file__).resolve().parent.parent
ASSETS = ROOT / "assets" / "fonts"
# Overridable only so the determinism claim above can be CHECKED rather than
# asserted: write to a scratch directory and diff against the committed files.
# Unset, it writes where the renderer reads from.
OUT_DIR = pathlib.Path(
    os.environ.get("NOTE4C_ATLAS_OUT", ROOT / "src" / "core" / "render" / "fonts")
)

ATLAS_SCHEMA_VERSION = 2

# Curated for a 400x300 four-colour panel read from across a room. Four
# families with genuinely different skeletons, so choosing one is a real
# choice and not a shade of the same letterform.
FAMILIES = {
    "inter": {
        "label": "Inter",
        "note": "Tall x-height grotesque drawn for screens. The densest of the "
                "three: fits the most words in a cell while staying readable.",
        "licence": "SIL Open Font License 1.1",
        "copyright": "Copyright 2020 The Inter Project Authors",
        "upstream": "https://github.com/rsms/inter",
        "vendored_from": "npm @expo-google-fonts/inter@0.4.2",
        "faces": {
            "regular": "inter/Inter_400Regular.ttf",
            "bold": "inter/Inter_700Bold.ttf",
        },
    },
    "atkinson": {
        "label": "Atkinson Hyperlegible",
        "note": "Drawn by the Braille Institute to maximise how far apart "
                "similar letters look. Deliberately distinguishes I l 1 and "
                "O 0, which is the failure mode of a small panel read at "
                "distance.",
        "licence": "SIL Open Font License 1.1",
        "copyright": "Copyright 2020 Braille Institute of America, Inc.",
        "upstream": "https://www.brailleinstitute.org/freefont",
        "vendored_from": "npm @expo-google-fonts/atkinson-hyperlegible@0.4.1",
        "faces": {
            "regular": "atkinson/AtkinsonHyperlegible_400Regular.ttf",
            "bold": "atkinson/AtkinsonHyperlegible_700Bold.ttf",
        },
    },
    "plexmono": {
        "label": "IBM Plex Mono",
        "note": "Fixed pitch. Every digit is the same width, so a column of "
                "temperatures or times lines up and a changing value does not "
                "shift the ones beside it.",
        "licence": "SIL Open Font License 1.1",
        "copyright": 'Copyright 2017 IBM Corp. with Reserved Font Name "Plex"',
        "upstream": "https://github.com/IBM/plex",
        "vendored_from": "npm @expo-google-fonts/ibm-plex-mono@0.4.1",
        "faces": {
            "regular": "plexmono/IBMPlexMono_400Regular.ttf",
            "bold": "plexmono/IBMPlexMono_700Bold.ttf",
        },
    },
    "poppins": {
        "label": "Poppins",
        "note": "Geometric sans with near-circular bowls and a single-storey a. "
                "The warmest of the four and the least like a dashboard: for a "
                "panel that is a note on the wall rather than an instrument.",
        "licence": "SIL Open Font License 1.1",
        "copyright": "Copyright 2020 The Poppins Project Authors "
                     "(https://github.com/itfoundry/Poppins)",
        "upstream": "https://github.com/itfoundry/Poppins",
        # Honest provenance: these two faces were copied from the Google Fonts
        # release of Poppins 4.004 already vendored in apps/aftermath on this
        # machine, not fetched. Their name tables say ITFO; Poppins Regular;
        # 4.004b8 and the licence text beside them is that release's own
        # OFL.txt, copied verbatim.
        "vendored_from": "Google Fonts release of Poppins 4.004 (name table: ITFO; 4.004b8)",
        "faces": {
            "regular": "poppins/Poppins_400Regular.ttf",
            "bold": "poppins/Poppins_700Bold.ttf",
        },
    },
}

# The size ladder offered in the inspector. Discrete because every size has to
# be rasterised and committed: an arbitrary size box would promise sizes that
# have no atlas. The steps are roughly a 1.22 ratio, which is far enough apart
# to be a visible choice and close enough to always have a usable neighbour.
SIZES = [11, 13, 15, 18, 22, 27, 34, 48, 64]

# Latin-1 printable, which covers French, plus the ellipsis the truncator uses.
CODEPOINTS = list(range(32, 127)) + list(range(160, 256)) + [0x2026]


def render_glyph(font: ImageFont.FreeTypeFont, char: str, pad: int) -> dict:
    """One glyph, hinted and grid-fitted, as 1bpp rows packed MSB first."""
    # A "1" target is what makes Pillow ask FreeType for FT_RENDER_MODE_MONO.
    canvas = Image.new("1", (pad * 3, pad * 3), 0)
    # Default anchor "la": x is the pen, y is the ascender line.
    ImageDraw.Draw(canvas).text((pad, pad), char, font=font, fill=1)
    box = canvas.getbbox()

    # Hinted advance, in the same rendering mode, so it is a whole number of
    # pixels and the pen never lands between two of them.
    advance = int(round(font.getlength(char, mode="1")))

    if box is None:
        return {"a": advance, "l": 0, "t": 0, "w": 0, "h": 0, "b": ""}

    left, top, right, bottom = box
    width, height = right - left, bottom - top
    pixels = canvas.crop(box).load()

    stride = (width + 7) // 8
    packed = bytearray(stride * height)
    for y in range(height):
        for x in range(width):
            if pixels[x, y]:
                packed[y * stride + (x >> 3)] |= 1 << (7 - (x & 7))

    return {
        "a": advance,
        "l": left - pad,
        "t": top - pad,
        "w": width,
        "h": height,
        "b": base64.b64encode(bytes(packed)).decode("ascii"),
    }


def build_atlas(path: pathlib.Path, family: str, weight: str, size: int) -> dict:
    font = ImageFont.truetype(str(path), size)
    ascent, descent = font.getmetrics()
    pad = size * 2 + 16
    return {
        "family": family,
        "weight": weight,
        "size": size,
        "ascent": ascent,
        "descent": descent,
        "lineHeight": ascent + descent,
        "glyphs": {
            str(codepoint): render_glyph(font, chr(codepoint), pad)
            for codepoint in CODEPOINTS
        },
    }


def sha256(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    missing = [
        str(ASSETS / rel)
        for meta in FAMILIES.values()
        for rel in meta["faces"].values()
        if not (ASSETS / rel).exists()
    ]
    if missing:
        for path in missing:
            print(f"Missing vendored face: {path}", file=sys.stderr)
        return 1

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    manifest: dict[str, dict] = {}

    for family, meta in FAMILIES.items():
        atlases: dict[str, dict] = {}
        for weight, rel in meta["faces"].items():
            for size in SIZES:
                atlases[f"{weight}-{size}"] = build_atlas(
                    ASSETS / rel, family, weight, size
                )
                print(f"  atlas {family} {weight}-{size}", file=sys.stderr)

        payload = {
            "schema_version": ATLAS_SCHEMA_VERSION,
            "family": family,
            "label": meta["label"],
            "raster": "freetype-mono-hinted",
            "licence": meta["licence"],
            "atlases": atlases,
        }
        target = OUT_DIR / f"{family}.json"
        # Deterministic output: regenerating on an unchanged input produces a
        # byte-identical file, so the committed atlases are reviewable.
        target.write_text(json.dumps(payload, sort_keys=True, separators=(",", ":")))
        print(f"  wrote {target.name} ({target.stat().st_size} bytes)", file=sys.stderr)

        manifest[family] = {
            "label": meta["label"],
            "note": meta["note"],
            "licence": meta["licence"],
            "copyright": meta["copyright"],
            "upstream": meta["upstream"],
            "vendored_from": meta["vendored_from"],
            "licence_file": f"assets/fonts/{family}/OFL.txt",
            "faces": {
                weight: {
                    "path": f"assets/fonts/{rel}",
                    "sha256": sha256(ASSETS / rel),
                }
                for weight, rel in meta["faces"].items()
            },
        }

    (OUT_DIR / "manifest.json").write_text(
        json.dumps(
            {
                "schema_version": ATLAS_SCHEMA_VERSION,
                "raster": "freetype-mono-hinted",
                "sizes": SIZES,
                "weights": ["regular", "bold"],
                "families": manifest,
            },
            indent=2,
            sort_keys=True,
        )
        + "\n"
    )
    print("  wrote manifest.json", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
