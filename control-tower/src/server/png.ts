import { deflateSync } from "node:zlib";
import {
  FRAME_HEIGHT,
  FRAME_WIDTH,
  PREVIEW_RGB_TRIPLETS,
} from "@/core/palette";
import { FrameBuffer, pack } from "@/core/frame";

/**
 * Minimal indexed-PNG encoder for frame previews and QA artefacts.
 *
 * The device format is already 2 bits per pixel with a 4-entry palette, which
 * is exactly a bit-depth-2 colour-type-3 PNG, so the packed bytes drop
 * straight into scanlines with no conversion and no colour drift.
 */

const SIGNATURE = Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]);

const CRC_TABLE = (() => {
  const table = new Uint32Array(256);
  for (let n = 0; n < 256; n += 1) {
    let c = n;
    for (let k = 0; k < 8; k += 1) {
      c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    }
    table[n] = c >>> 0;
  }
  return table;
})();

function crc32(buffer: Buffer): number {
  let c = 0xffffffff;
  for (let i = 0; i < buffer.length; i += 1) {
    c = (CRC_TABLE[(c ^ (buffer[i] as number)) & 0xff] as number) ^ (c >>> 8);
  }
  return (c ^ 0xffffffff) >>> 0;
}

function chunk(type: string, data: Buffer): Buffer {
  const length = Buffer.alloc(4);
  length.writeUInt32BE(data.length, 0);
  const body = Buffer.concat([Buffer.from(type, "ascii"), data]);
  const crc = Buffer.alloc(4);
  crc.writeUInt32BE(crc32(body), 0);
  return Buffer.concat([length, body, crc]);
}

export function framePng(fb: FrameBuffer | Uint8Array): Buffer {
  const packed = fb instanceof FrameBuffer ? pack(fb) : fb;
  const bytesPerRow = FRAME_WIDTH / 4; // 2bpp, 100 bytes

  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(FRAME_WIDTH, 0);
  ihdr.writeUInt32BE(FRAME_HEIGHT, 4);
  ihdr.writeUInt8(2, 8); // bit depth
  ihdr.writeUInt8(3, 9); // colour type: indexed
  ihdr.writeUInt8(0, 10); // compression
  ihdr.writeUInt8(0, 11); // filter
  ihdr.writeUInt8(0, 12); // interlace

  const plte = Buffer.alloc(PREVIEW_RGB_TRIPLETS.length * 3);
  PREVIEW_RGB_TRIPLETS.forEach(([r, g, b], index) => {
    plte.writeUInt8(r, index * 3);
    plte.writeUInt8(g, index * 3 + 1);
    plte.writeUInt8(b, index * 3 + 2);
  });

  const raw = Buffer.alloc((bytesPerRow + 1) * FRAME_HEIGHT);
  for (let y = 0; y < FRAME_HEIGHT; y += 1) {
    const offset = y * (bytesPerRow + 1);
    raw.writeUInt8(0, offset); // filter type: none
    Buffer.from(
      packed.buffer,
      packed.byteOffset + y * bytesPerRow,
      bytesPerRow,
    ).copy(raw, offset + 1);
  }

  return Buffer.concat([
    SIGNATURE,
    chunk("IHDR", ihdr),
    chunk("PLTE", plte),
    chunk("IDAT", deflateSync(raw, { level: 9 })),
    chunk("IEND", Buffer.alloc(0)),
  ]);
}

export function framePngDataUrl(fb: FrameBuffer | Uint8Array): string {
  return `data:image/png;base64,${framePng(fb).toString("base64")}`;
}
