import zlib from "node:zlib";
import { expect, test, type Page } from "@playwright/test";
import {
  createEmptyDashboard,
  previewPixels,
  shot,
  signIn,
  stablePixels,
} from "./helpers";

/**
 * The Image module, driven the way an owner would: drop a picture, watch it
 * become a dithered tile, switch Photo and Poster, and save.
 *
 * The picture is generated here rather than committed, so no test photograph
 * is a fact about anyone. It never leaves the browser: the module stores the
 * dithered result, so a reload paints the identical tile with no re-dither.
 */

const CRC_TABLE = (() => {
  const table = new Uint32Array(256);
  for (let n = 0; n < 256; n += 1) {
    let c = n;
    for (let k = 0; k < 8; k += 1) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    table[n] = c >>> 0;
  }
  return table;
})();

function crc32(buf: Buffer): number {
  let c = 0xffffffff;
  for (const byte of buf) c = (CRC_TABLE[(c ^ byte) & 0xff] as number) ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

function chunk(type: string, data: Buffer): Buffer {
  const len = Buffer.alloc(4);
  len.writeUInt32BE(data.length, 0);
  const typeBuf = Buffer.from(type, "ascii");
  const crc = Buffer.alloc(4);
  crc.writeUInt32BE(crc32(Buffer.concat([typeBuf, data])), 0);
  return Buffer.concat([len, typeBuf, data, crc]);
}

/** A small colourful gradient PNG, so the dither has reds and yellows to keep. */
function samplePng(width = 240, height = 180): Buffer {
  const raw = Buffer.alloc((width * 3 + 1) * height);
  let p = 0;
  const cx = width * 0.6;
  const cy = height * 0.4;
  for (let y = 0; y < height; y += 1) {
    raw[p++] = 0; // filter: none
    for (let x = 0; x < width; x += 1) {
      const v = y / height;
      let r = 60 + v * 195;
      let g = 90 + v * 120;
      let b = 200 - v * 170;
      const d = Math.hypot(x - cx, y - cy) / (width * 0.2);
      if (d < 1) {
        const t = 1 - d;
        r += (255 - r) * t;
        g += (245 - g) * t;
        b += (40 - b) * t;
      }
      raw[p++] = Math.max(0, Math.min(255, Math.round(r)));
      raw[p++] = Math.max(0, Math.min(255, Math.round(g)));
      raw[p++] = Math.max(0, Math.min(255, Math.round(b)));
    }
  }
  const sig = Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]);
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(width, 0);
  ihdr.writeUInt32BE(height, 4);
  ihdr[8] = 8; // bit depth
  ihdr[9] = 2; // colour type: truecolour
  const idat = zlib.deflateSync(raw);
  return Buffer.concat([
    sig,
    chunk("IHDR", ihdr),
    chunk("IDAT", idat),
    chunk("IEND", Buffer.alloc(0)),
  ]);
}

test.beforeEach(async ({ page }) => {
  await signIn(page);
});

async function openEmptyDesigner(page: Page, name: string): Promise<string> {
  const { id } = await createEmptyDashboard(page, name);
  await page.goto(`/dashboards/${id}/edit`);
  await expect(page.getByTestId("designer-stage")).toBeVisible();
  return id;
}

async function upload(page: Page): Promise<void> {
  await page.getByTestId("opt-source-upload").setInputFiles({
    name: "sample.png",
    mimeType: "image/png",
    buffer: samplePng(),
  });
}

test("an image is uploaded, dithered, and shown on the panel", async ({
  page,
}, info) => {
  await openEmptyDesigner(page, `Image ${info.project.name}`);
  await page.getByTestId("add-image").click();
  await expect(page.getByTestId("module-image")).toBeVisible();

  // No picture yet: an explicit empty state, not a blank rectangle.
  await expect(page.getByTestId("opt-source-upload")).toBeAttached();
  const empty = await previewPixels(page);

  await upload(page);
  await expect(page.getByTestId("opt-source-preview")).toBeVisible();
  await page.waitForTimeout(300);
  const photo = await previewPixels(page);
  expect(photo).not.toBe(empty);
  await shot(page, "image-photo", info.project.name);

  // Poster is a different picture from Photo.
  await page.getByTestId("opt-source-mode-poster").click();
  await page.waitForTimeout(300);
  const poster = await previewPixels(page);
  expect(poster).not.toBe(photo);
  await shot(page, "image-poster", info.project.name);

  // Fit changes it too.
  await page.getByTestId("opt-source-mode-photo").click();
  await page.getByTestId("opt-source-fit-contain").click();
  await page.waitForTimeout(300);
  expect(await previewPixels(page)).not.toBe(photo);
});

test("colour amount runs from black-and-white to expressive", async ({
  page,
}, info) => {
  await openEmptyDesigner(page, `Image colour ${info.project.name}`);
  await page.getByTestId("add-image").click();
  await upload(page);
  await page.waitForTimeout(300);

  const expressive = await previewPixels(page);
  await page.getByTestId("opt-source-colourAmount").fill("0");
  await page.waitForTimeout(300);
  expect(await previewPixels(page)).not.toBe(expressive);
});

test("a dithered image survives a save and a reload with no re-dither", async ({
  page,
}, info) => {
  const id = await openEmptyDesigner(page, `Image persist ${info.project.name}`);
  await page.getByTestId("add-image").click();
  await upload(page);
  await page.getByTestId("opt-source-mode-poster").click();
  await page.waitForTimeout(300);
  const before = await previewPixels(page);

  await page.getByTestId("save-version").click();
  await expect(page.getByTestId("version-rail")).toContainText("v2");

  await page.goto(`/dashboards/${id}/edit`);
  await page.getByTestId("module-image").click();
  // The stored tile is blitted verbatim, so the panel is byte-identical.
  expect(await stablePixels(page)).toBe(before);
});
