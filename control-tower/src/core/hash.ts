/**
 * One hashing interface for both runtimes. WebCrypto is present in Node 22 and
 * in every browser context the tower runs in (loopback is a secure context),
 * so a single async implementation stays byte-identical on both sides and the
 * renderer can be shared verbatim between server and preview.
 *
 * Server code that needs a synchronous digest uses src/server/hash.ts, which
 * is the node:crypto path.
 */

const HEX = "0123456789abcdef";

export function toHex(bytes: ArrayBuffer | Uint8Array): string {
  const view = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
  let out = "";
  for (let i = 0; i < view.length; i += 1) {
    const byte = view[i] as number;
    out += HEX[byte >> 4];
    out += HEX[byte & 15];
  }
  return out;
}

export async function sha256Hex(bytes: Uint8Array): Promise<string> {
  const subtle = globalThis.crypto?.subtle;
  if (!subtle) {
    throw new Error(
      "WebCrypto is unavailable in this runtime; server code should use src/server/hash.ts",
    );
  }
  const copy = new Uint8Array(bytes.length);
  copy.set(bytes);
  const digest = await subtle.digest("SHA-256", copy);
  return toHex(digest);
}
