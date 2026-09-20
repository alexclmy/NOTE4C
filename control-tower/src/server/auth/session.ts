import { createHmac, randomBytes, timingSafeEqual } from "node:crypto";
import fs from "node:fs";
import { FILE_MODE, ensureDataRoot, paths } from "../store/paths";
import { writeFileAtomic } from "../store/atomicFile";

export const SESSION_COOKIE = "note4c_tower_session";
export const CSRF_COOKIE = "note4c_tower_csrf";
export const CSRF_HEADER = "x-csrf-token";

/** Long enough to be usable on a home Mac, short enough to expire. */
export const SESSION_TTL_SECONDS = 12 * 60 * 60;

function sessionSecret(): Buffer {
  ensureDataRoot();
  const file = paths.secret("session-secret");
  try {
    const existing = fs.readFileSync(file, "utf8").trim();
    if (existing.length >= 64) return Buffer.from(existing, "hex");
  } catch {
    // First run: mint one below.
  }
  const secret = randomBytes(32);
  writeFileAtomic(file, secret.toString("hex"), FILE_MODE);
  return secret;
}

function b64url(input: Buffer | string): string {
  return Buffer.from(input)
    .toString("base64")
    .replace(/\+/g, "-")
    .replace(/\//g, "_")
    .replace(/=+$/, "");
}

function fromB64url(value: string): Buffer {
  return Buffer.from(value.replace(/-/g, "+").replace(/_/g, "/"), "base64");
}

export interface SessionPayload {
  sub: string;
  iat: number;
  exp: number;
}

export function issueSession(now: Date = new Date()): {
  token: string;
  csrf: string;
  expiresAt: Date;
} {
  const issuedAt = Math.floor(now.getTime() / 1000);
  const payload: SessionPayload = {
    sub: "operator",
    iat: issuedAt,
    exp: issuedAt + SESSION_TTL_SECONDS,
  };
  const body = b64url(JSON.stringify(payload));
  const signature = b64url(
    createHmac("sha256", sessionSecret()).update(body).digest(),
  );
  return {
    token: `${body}.${signature}`,
    csrf: randomBytes(32).toString("hex"),
    expiresAt: new Date(payload.exp * 1000),
  };
}

export function verifySession(
  token: string | undefined,
  now: Date = new Date(),
): SessionPayload | null {
  if (!token) return null;
  const parts = token.split(".");
  if (parts.length !== 2) return null;
  const [body, signature] = parts as [string, string];

  const expected = createHmac("sha256", sessionSecret()).update(body).digest();
  const presented = fromB64url(signature);
  if (presented.length !== expected.length) return null;
  if (!timingSafeEqual(presented, expected)) return null;

  let payload: SessionPayload;
  try {
    payload = JSON.parse(fromB64url(body).toString("utf8")) as SessionPayload;
  } catch {
    return null;
  }
  if (typeof payload.exp !== "number") return null;
  if (payload.exp * 1000 <= now.getTime()) return null;
  return payload;
}

/**
 * Double-submit CSRF. The cookie is readable by the page so the client can
 * echo it in a header; an attacker's page can send the cookie but cannot read
 * it, so it cannot produce the matching header.
 */
export function csrfMatches(
  cookieValue: string | undefined,
  headerValue: string | null | undefined,
): boolean {
  if (!cookieValue || !headerValue) return false;
  if (cookieValue.length !== headerValue.length) return false;
  return timingSafeEqual(
    Buffer.from(cookieValue, "utf8"),
    Buffer.from(headerValue, "utf8"),
  );
}
