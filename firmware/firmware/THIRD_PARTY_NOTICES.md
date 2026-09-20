# Third-party notices — autonomous rendering

This file records what the autonomous-rendering work reuses from outside this
repository, and under what terms. It covers the files added for that feature;
the firmware's own upstream lineage is recorded in `../ATTRIBUTION.md` and is
unchanged by this work.

Every entry below was read from the file it describes — the source file, its
licence text, or the provider's own stated terms — rather than recalled.

## eMini Home 0.4.0

- **Upstream:** `/Users/marvin/projects/open-source-lab/emini-home`, pinned at
  commit `05ec313f86b1ecfb6f8ecc692fb1f9ef00e78544`, read-only.
- **Licence:** MIT, a single copyright holder: **© 2026 Tomasz Fiedoruk**.
- **Full licence text:** reproduced at the end of this section, because eMini
  carries its MIT grant at the repository root and almost never as a per-file
  header. A file copied out of such a project takes the licence text with it or
  it travels with no licence at all.

eMini Home targets the same NOTE4C panel this firmware runs on — its own
driver is adapted from our upstream (`LazyYoun@51812e4`) with MIT attribution
in `home_panel.c`. This section closes that loop in the other direction.

### What is adapted, and where

| Our file | What was taken | eMini source |
| --- | --- | --- |
| `main/common/autonomy_compose.cc` | The ordered 4×4 Bayer kernel and the `threshold < level` comparison in `Canvas::Dither()`. The matrix itself is the standard ordered-dither matrix; what is genuinely eMini's is the decision to dither on this 2bpp panel at all, and the shape of the helper. | `home_render.c:45-104` (`mix`, `bayer[4][4]`, `pixel`) |
| `main/common/autonomy_compose.cc` | The descending auto-fit discipline in `FitSize()`: measure with the same routine that will paint, and walk a size ladder downwards rather than scaling a bitmap. **No code is copied** — our text engine is not eMini's — so this is an adaptation of a rule, not of an implementation. | `home_render.c:794-889` (`poster_layout`) |
| `main/common/autonomy_compose.cc` | The sparkline-with-dithered-band idea in `DrawCurve()`. The arithmetic is ours and is entirely integer, for the parity reasons in `autonomy_compose.h`. | `home_render.c` (`forecast_graph`) |
| `main/common/openmeteo_parse.cc` | Validation *discipline* only: reject rather than guess on units, refuse duplicate keys, treat a partial document as no document. **No code is copied**: eMini reads MET Norway, whose schema is not Open-Meteo's. | `home_fetch.c`, `home_parse.c` |

The three compositions are named `editorial`, `flow` and `focus`. Their visual
grammar has a real filiation with eMini's Print, Rhythm and Atlas
respectively — typographic, data-forward, scenic — but the data, the labels,
the language, the typography and every coordinate are ours. The filiation is
recorded here because it is real, not because any code was taken.

### What was deliberately NOT taken

Recorded because "we did not use it" is a claim that deserves to be as precise
as "we did":

- **eMini's embedded font (`home_font.c`).** It is Atkinson Hyperlegible under
  the SIL Open Font License — a *different* obligation from MIT, not
  relicensable under it — and its generator is not in the repository. Our glyph
  data is generated from the OFL faces this project already vendors; see below.
- **The MET Norway parser (`home_parse.c:329-425`).** A different schema. We
  port the validation contract from our own tower instead.
- **The HTTPS fetcher and RFC 9111 cache (`home_fetch.c`, ~730 lines).** Well
  built, and built to serve an arbitrary URL. Our device has exactly one
  compile-time origin, so `esp_http_client` with a one-domain allowlist is the
  smaller and more auditable answer. Worth revisiting if a second connector
  ever arrives.
- **The screen scheduler and anything power-related.** eMini has no sleep at
  all — it keeps Wi-Fi up permanently. Our `power_policy` is a different and
  stricter contract and stays.
- **The partition layout, the embedded web UI, mDNS, the QR helper, the
  timezone table and the NVS A/B store.** Each duplicates something this
  firmware already has, or is outside V1.

### MIT licence text, as it appears in eMini Home

```
MIT License

Copyright (c) 2026 Tomasz Fiedoruk

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Inter — glyph data in `autonomy_font_data.{h,cc}`

- **Licence:** SIL Open Font License, Version 1.1.
- **Copyright:** Copyright 2020 The Inter Project Authors
  (<https://github.com/rsms/inter>), as stated in the family's own `OFL.txt`.
- **Licence text:** vendored in the Control Tower repository at
  `assets/fonts/inter/OFL.txt`, beside the `.ttf` files themselves.

`main/common/autonomy_font_data.{h,cc}` are **generated** files containing 1bpp
glyph bitmaps derived from Inter. They are not hand-edited; they are emitted by
`tools/gen_firmware_font.py` in the Control Tower repository, which re-encodes a
subset of the atlases that repository already commits
(`src/core/render/fonts/inter.json`). Nothing is rasterised in this repository.

The OFL permits redistribution of the fonts and of derived works under the same
licence, which is why these files can live in git. Two things follow from that
and are worth stating plainly:

1. **The OFL obligation is separate from MIT.** The surrounding firmware is
   MIT; this glyph data is OFL, and the two are not interchangeable. The
   generated files carry a header saying so.
2. **Nothing here is distributed as a font under a reserved name.** These are
   bitmaps for one panel at four sizes, not a font, and they are not offered
   under the name Inter.

The same generator run emits the tower's
`src/core/autonomyPreview/fontData.ts` from the same source in the same pass.
That is deliberate: the two renderers are compared byte for byte, and glyph
data that came from two runs could differ.

## Open-Meteo

- **Terms:** the forecast data is published under **CC BY 4.0**, per
  Open-Meteo's own stated terms.
- **Attribution in the product:** the autonomously composed panel prints
  "Météo Open-Meteo" on its provenance footer when `provenance_line` is on, and
  the tower credits Open-Meteo in its own `NOTICE`.

The device contacts exactly one third-party origin, `https://api.open-meteo.com`,
as a compile-time constant. No API key is used or needed. When
`provenance_line` is turned off the credit remains in the tower and in this
file; for a private, unpublished installation that is a defensible reading of
CC BY, and it should be revisited before any publication.

## QR Code generator library — `main/rawdraw/qrcodegen.{c,h}`

- **Author:** Project Nayuki.
- **Licence:** MIT.
- **Upstream:** https://www.nayuki.io/page/qr-code-generator-library
- **How it got here:** copied byte-identical from
  `managed_components/espressif2022__esp_emote_gfx/src/lib/qrcode/`, which is
  itself a verbatim copy of Nayuki's C implementation. The licence header at the
  top of both files is the upstream one and is unmodified.

### Why it is vendored rather than used where it sits

Both QR libraries already in `managed_components/` (LVGL's `lv_qrcode` and
`esp_emote_gfx`'s `gfx_qrcode`) wrap this same core behind a widget or canvas
runtime that this board does not build — it renders in pure rawdraw mode, with
`HAVE_LVGL` undefined. The core itself needs nothing but `<stdbool.h>`,
`<stddef.h>` and `<stdint.h>`.

It is kept **unmodified**, so a future upstream revision is a drop-in replace
rather than a merge. Everything this project decided — medium error correction,
a four-module quiet zone, a three-pixel-per-module floor, black-and-white
modules only, and refusing to draw at all rather than drawing something
unscannable — is in `main/rawdraw/qr_render.cc`, which is ours.

`tests/host/run.sh` compiles it with the C compiler rather than the C++ one for
the same reason: a vendored C file must stay valid C, and must not acquire C++
linkage that its caller's `extern "C"` declaration would not match.

## Octopus mark — `main/rawdraw/octopus_mark.{h,cc}`

Original artwork, drawn from `rawdraw` primitives. Not traced from, derived
from, or inspired by any third-party asset; no font, glyph or image file is
involved. Listed here only so the absence of an obligation is on the record
rather than assumed.

## What this firmware does *not* reach

Stated because the previous comment in `dashboard_build_config.h` claimed the
minimal build contacted no third-party service at all, and that claim stopped
being true with this feature. It now names the one service, which is the
honest form of the same sentence.

Outside of Open-Meteo and the existing NTP pools (`pool.ntp.org`,
`time.nist.gov`), the autonomous path contacts nothing. There is no URL field
anywhere in the autonomy profile, at any depth; the origin is not something a
pushed document can move.
