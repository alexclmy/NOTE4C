import fs from "node:fs";
import { FILE_MODE, ensureDataRoot, paths } from "../store/paths";
import { writeFileAtomic } from "../store/atomicFile";
import { bridgeTokenPath } from "@/server/config";

/**
 * The device token.
 *
 * Stored at 0600 in the tower's private secrets directory. It is never
 * returned by an API route, never written to the ledger or the audit log, and
 * never rendered. Only the device client reads it, and only to set the
 * X-Auth-Token header.
 */

/** The composer bridge's existing credential, imported only with consent. */
export { bridgeTokenPath };

export function deviceTokenPath(): string {
  return paths.secret("device-token");
}

export function deviceTokenIsSet(): boolean {
  ensureDataRoot();
  try {
    return readDeviceToken() !== null;
  } catch {
    return false;
  }
}

export function readDeviceToken(): string | null {
  ensureDataRoot();
  try {
    const value = fs.readFileSync(deviceTokenPath(), "utf8").trim();
    return /^[0-9a-f]{64}$/.test(value) ? value : null;
  } catch {
    return null;
  }
}

export function writeDeviceToken(token: string): void {
  const value = token.trim();
  if (!/^[0-9a-f]{64}$/.test(value)) {
    throw new Error("A device token must be 64 lowercase hexadecimal characters");
  }
  ensureDataRoot();
  writeFileAtomic(deviceTokenPath(), value, FILE_MODE);
}

export function bridgeTokenAvailable(path: string = bridgeTokenPath()): boolean {
  try {
    return /^[0-9a-f]{64}$/.test(fs.readFileSync(path, "utf8").trim());
  } catch {
    return false;
  }
}

/**
 * Copy the composer bridge's token into the tower's own secrets directory.
 * The caller must already have shown a consent dialog naming this exact path.
 * The value is never displayed, logged or returned.
 */
export function importBridgeToken(path: string = bridgeTokenPath()): void {
  let value: string;
  try {
    value = fs.readFileSync(path, "utf8").trim();
  } catch {
    throw new Error(`No device token found at ${path}`);
  }
  writeDeviceToken(value);
}

export function forgetDeviceToken(): void {
  try {
    fs.rmSync(deviceTokenPath(), { force: true });
  } catch {
    // Nothing to forget.
  }
}
