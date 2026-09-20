/**
 * Write parity fixtures for tools/parity_check.py.
 *
 * For each fixture this emits <name>.png (the frame, indexed, exact device
 * palette) and <name>.bin (this renderer's packed 30000 bytes). The Python
 * side re-packs the PNG with the composer's own pack() and compares.
 *
 *   npx tsx tools/export_parity_fixtures.ts <output-dir>
 */
import fs from "node:fs";
import path from "node:path";
import { FrameBuffer, pack } from "@/core/frame";
import { BLACK, RED, WHITE, YELLOW } from "@/core/palette";
import { framePng } from "@/server/png";
import { font } from "@/core/render/fonts";
import { applyPalettePolicy } from "@/core/theme";

const fixtures: Record<string, () => FrameBuffer> = {
  "all-white": () => new FrameBuffer(WHITE),
  "all-black": () => new FrameBuffer(BLACK),
  "all-yellow": () => new FrameBuffer(YELLOW),
  "all-red": () => new FrameBuffer(RED),
  "quadrants": () => {
    const fb = new FrameBuffer(WHITE);
    fb.fillRect(0, 0, 200, 150, BLACK);
    fb.fillRect(200, 0, 200, 150, YELLOW);
    fb.fillRect(0, 150, 200, 150, RED);
    return fb;
  },
  "odd-offsets": () => {
    // Deliberately unaligned rectangles so any 2bpp sub-byte error shows up.
    const fb = new FrameBuffer(WHITE);
    fb.fillRect(1, 1, 1, 1, BLACK);
    fb.fillRect(3, 5, 7, 3, RED);
    fb.fillRect(397, 297, 3, 3, YELLOW);
    fb.vline(199, 0, 300, BLACK);
    fb.hline(0, 149, 400, RED);
    return fb;
  },
  "diagonal": () => {
    const fb = new FrameBuffer(WHITE);
    for (let i = 0; i < 300; i += 1) {
      fb.set(i, i, BLACK);
      fb.set(i + 1, i, RED);
      fb.set(i + 2, i, YELLOW);
    }
    return fb;
  },
  "text-sample": () => {
    const fb = new FrameBuffer(WHITE);
    fb.drawText(font("inter", "bold", 22), 12, 7, "KITCHEN PANEL", BLACK);
    fb.drawText(font("inter", "regular", 13), 154, 13, "LES PROCHAINES 24H", BLACK);
    fb.drawText(font("atkinson", "regular", 18), 12, 65, "Meteo indisponible", RED);
    fb.drawText(font("plexmono", "regular", 11), 293, 280, "MAJ 11/09 01:07", BLACK);
    return fb;
  },
  /** The fourth family, so its atlas is in the byte-level cross-check too. */
  "poppins-text": () => {
    const fb = new FrameBuffer(WHITE);
    fb.drawText(font("poppins", "bold", 27), 10, 10, "KITCHEN PANEL", BLACK);
    fb.drawText(font("poppins", "regular", 15), 10, 60, "Collect the parcel", BLACK);
    fb.drawText(font("poppins", "regular", 11), 10, 100, "Ça, ÔÙÏ, 19°C …", RED);
    return fb;
  },
  /**
   * Pictograms, including one the set does not cover. These write yellow and
   * red from a path that is not text and not a sprite, so the packer has to
   * agree about them the same way it agrees about everything else.
   */
  "pictograms": () => {
    const fb = new FrameBuffer(WHITE);
    fb.drawText(font("inter", "regular", 15), 8, 8, "☀️ ❤️ ⚠️ 🥚 🐔 ⏰ 🦄", BLACK);
    fb.drawText(font("inter", "regular", 27), 8, 60, "🎉 🏠 🚗 ⭐ 🍁", BLACK);
    return fb;
  },
  /**
   * A frame after the palette policy has run with red and yellow disabled.
   * Nothing but black and white may survive, and it still has to pack byte for
   * byte the way the composer would pack it.
   */
  "palette-fallback": () => {
    const fb = new FrameBuffer(WHITE);
    fb.fillRect(10, 10, 180, 80, RED);
    fb.fillRect(210, 10, 180, 80, YELLOW);
    fb.drawText(font("inter", "bold", 22), 12, 140, "Accent désactivé", RED);
    applyPalettePolicy(fb.pixels, {
      black: true,
      white: true,
      red: false,
      yellow: false,
    });
    return fb;
  },
};

const outDir = process.argv[2];
if (!outDir) {
  console.error("usage: tsx tools/export_parity_fixtures.ts <output-dir>");
  process.exit(2);
}
fs.mkdirSync(outDir, { recursive: true });

for (const [name, build] of Object.entries(fixtures)) {
  const fb = build();
  fs.writeFileSync(path.join(outDir, `${name}.png`), framePng(fb));
  fs.writeFileSync(path.join(outDir, `${name}.bin`), pack(fb));
}

console.log(Object.keys(fixtures).join("\n"));
