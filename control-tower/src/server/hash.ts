import { createHash, timingSafeEqual } from "node:crypto";

/** Synchronous digest for server paths (ledger, push pipeline, mock device). */
export function sha256HexSync(bytes: Uint8Array | Buffer | string): string {
  return createHash("sha256").update(bytes).digest("hex");
}

/**
 * Constant-time comparison of two hex or ASCII secrets. Length differences are
 * unavoidably observable, so compare fixed-size digests of the inputs instead
 * of the raw values.
 */
export function secretEquals(a: string, b: string): boolean {
  const da = createHash("sha256").update(a, "utf8").digest();
  const db = createHash("sha256").update(b, "utf8").digest();
  return timingSafeEqual(da, db);
}
