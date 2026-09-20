import { randomBytes, scryptSync, timingSafeEqual } from "node:crypto";
import fs from "node:fs";
import { z } from "zod";
import { readDocument, writeDocument, type DocumentSpec } from "../store/atomicFile";
import { ensureDataRoot, paths } from "../store/paths";

/**
 * Tower passphrase.
 *
 * scrypt with per-install parameters recorded alongside the hash, so the cost
 * can be raised later without stranding an existing passphrase. The plaintext
 * is never stored, never logged, and never returned by any route.
 */

export const MIN_PASSPHRASE_LENGTH = 12;

const SCRYPT_N = 2 ** 15;
const SCRYPT_R = 8;
const SCRYPT_P = 1;
const KEY_LENGTH = 64;

const PassphraseRecordSchema = z.object({
  schema_version: z.literal(1),
  algorithm: z.literal("scrypt"),
  N: z.number().int().positive(),
  r: z.number().int().positive(),
  p: z.number().int().positive(),
  keyLength: z.number().int().positive(),
  salt: z.string(),
  hash: z.string(),
  createdAt: z.string(),
});
type PassphraseRecord = z.infer<typeof PassphraseRecordSchema>;

const SPEC: DocumentSpec<PassphraseRecord> = {
  schema: PassphraseRecordSchema,
  version: 1,
};

export class WeakPassphraseError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "WeakPassphraseError";
  }
}

function derive(
  passphrase: string,
  salt: Buffer,
  record: Pick<PassphraseRecord, "N" | "r" | "p" | "keyLength">,
): Buffer {
  return scryptSync(passphrase.normalize("NFKC"), salt, record.keyLength, {
    N: record.N,
    r: record.r,
    p: record.p,
    // scrypt needs memory proportional to N*r*128; raise the ceiling to match.
    maxmem: 256 * record.N * record.r,
  });
}

export function passphraseIsSet(): boolean {
  ensureDataRoot();
  return fs.existsSync(paths.secret("passphrase"));
}

export function setPassphrase(passphrase: string): void {
  if (passphrase.length < MIN_PASSPHRASE_LENGTH) {
    throw new WeakPassphraseError(
      `The passphrase must be at least ${MIN_PASSPHRASE_LENGTH} characters`,
    );
  }
  ensureDataRoot();
  const salt = randomBytes(32);
  const params = { N: SCRYPT_N, r: SCRYPT_R, p: SCRYPT_P, keyLength: KEY_LENGTH };
  const record: PassphraseRecord = {
    schema_version: 1,
    algorithm: "scrypt",
    ...params,
    salt: salt.toString("hex"),
    hash: derive(passphrase, salt, params).toString("hex"),
    createdAt: new Date().toISOString(),
  };
  writeDocument(paths.secret("passphrase"), SPEC, record);
}

export function verifyPassphrase(passphrase: string): boolean {
  ensureDataRoot();
  const record = readDocument(paths.secret("passphrase"), SPEC);
  if (!record) return false;
  const expected = Buffer.from(record.hash, "hex");
  const actual = derive(passphrase, Buffer.from(record.salt, "hex"), record);
  if (expected.length !== actual.length) return false;
  return timingSafeEqual(expected, actual);
}
