# Paperwake

A local-first dashboard hub for a battery-powered, four-color e-paper panel.

Paperwake turns a [ZECTRIX ESP32-S3 e-paper panel](https://wiki.zectrix.com)
(the NOTE4C) into a display you can put wherever you like — a desk, a wall, a
hallway, the fridge door. Weather, calendar, countdowns, notes: composed on
your own machine and pushed to the panel over your LAN. No cloud, no account,
loopback only. The panel sleeps on battery and wakes on a timer; the composer
catches that wake and lands a fresh frame before it sleeps again.

It has two halves:

| | What it is | Stack |
|---|---|---|
| [**`firmware/`**](firmware/) | What runs on the panel: the render pipeline, the LAN API, power management, and the four-color RawDraw UI. | ESP-IDF (C/C++), ESP32-S3 |
| [**`control-tower/`**](control-tower/) | What runs on your machine: a paper-styled web app to design a dashboard, preview exactly what will be painted, push it, and read back what the panel says happened. | Next.js, TypeScript |

## What makes it different

- **The preview is the renderer.** The composer paints a dashboard with the same
  deterministic pipeline the panel uses, so what you see on screen is what the
  e-paper shows — down to the four-color dithering.
- **Honest delivery.** Every push is recorded in an append-only ledger with a
  real state: `queued`, `sent`, `verified_displayed`, `uncertain`, `failed`. A
  panel that was asleep is never reported as a failure, and a frame is never
  claimed on the glass until the device confirms its digest.
- **Battery autonomy.** The panel deep-sleeps and wakes on its own timer for a
  short window. The tower polls just often enough to catch that window and
  deliver the queued frame, then leaves the panel alone until the next one.
- **Four colours, done carefully.** Black, white, red and yellow (BWRY), with a
  dithering rule that never renders red or yellow finer than two pixels so the
  panel stays legible.

## Quick start

Each half has its own guide:

- **Control Tower** — [`control-tower/README.md`](control-tower/README.md) and
  [`control-tower/QUICKSTART.md`](control-tower/QUICKSTART.md). Runs on Node; a
  built-in mock device lets you try the whole thing with no hardware.
- **Firmware** — [`firmware/README.md`](firmware/README.md). Built with ESP-IDF
  for the ESP32-S3 target.

You can run the Control Tower against its mock device and never touch a panel —
that is the fastest way to see what Paperwake is.

## Provenance

The firmware is a consolidated fork of an upstream ESP32-S3 e-paper project; its
lineage, base revision and build hashes are documented in
[`firmware/ATTRIBUTION.md`](firmware/ATTRIBUTION.md) and
[`firmware/PROVENANCE-MANIFEST.md`](firmware/PROVENANCE-MANIFEST.md). The Control
Tower is original work.

## Author

Built by [**@alexclmy**](https://github.com/alexclmy). Say hi on Twitter/X:
[**@ytiralugins**](https://x.com/ytiralugins).

## License

[MIT](LICENSE). The firmware retains its upstream copyright notice in
[`firmware/LICENSE`](firmware/LICENSE) alongside this project's; see
`firmware/ATTRIBUTION.md` for the fork's provenance.

---

Paperwake is an **unofficial**, personal project. It is not affiliated with,
endorsed by, or supported by the panel's vendor.
