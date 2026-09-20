import { afterEach, beforeEach, describe, expect, it } from "vitest";
import fs from "node:fs";
import {
  MIN_PASSPHRASE_LENGTH,
  WeakPassphraseError,
  passphraseIsSet,
  setPassphrase,
  verifyPassphrase,
} from "@/server/auth/passphrase";
import {
  CSRF_HEADER,
  SESSION_TTL_SECONDS,
  csrfMatches,
  issueSession,
  verifySession,
} from "@/server/auth/session";
import {
  LOGIN_ATTEMPT_LIMIT,
  checkLoginRate,
  clearLoginRate,
  recordFailedLogin,
} from "@/server/auth/rateLimit";
import {
  BindRefusedError,
  assertBindAllowed,
  bindAllowed,
  isLoopbackHost,
} from "@/server/auth/bindGuard";
import { paths } from "@/server/store/paths";
import { modeOf, useTempDataRoot } from "./helpers/tempRoot";

let temp: ReturnType<typeof useTempDataRoot>;

beforeEach(() => {
  temp = useTempDataRoot();
  clearLoginRate();
});

afterEach(() => {
  temp.dispose();
});

describe("passphrase", () => {
  it("reports when none is set", () => {
    expect(passphraseIsSet()).toBe(false);
    expect(verifyPassphrase("anything at all")).toBe(false);
  });

  it("round-trips a correct passphrase", () => {
    setPassphrase("correct horse battery staple");
    expect(passphraseIsSet()).toBe(true);
    expect(verifyPassphrase("correct horse battery staple")).toBe(true);
  });

  it("refuses a wrong passphrase", () => {
    setPassphrase("correct horse battery staple");
    expect(verifyPassphrase("Correct horse battery staple")).toBe(false);
    expect(verifyPassphrase("")).toBe(false);
    expect(verifyPassphrase("correct horse battery stapl")).toBe(false);
  });

  it("refuses a passphrase that is too short", () => {
    expect(() => setPassphrase("short")).toThrow(WeakPassphraseError);
    expect(MIN_PASSPHRASE_LENGTH).toBeGreaterThanOrEqual(12);
  });

  it("never stores the plaintext", () => {
    setPassphrase("correct horse battery staple");
    const raw = fs.readFileSync(paths.secret("passphrase"), "utf8");
    expect(raw).not.toContain("correct horse");
    expect(raw).not.toContain("battery");
    const parsed = JSON.parse(raw);
    expect(parsed.algorithm).toBe("scrypt");
    expect(parsed.hash).toMatch(/^[0-9a-f]+$/);
    expect(parsed.salt).toMatch(/^[0-9a-f]+$/);
  });

  it("writes the record at 0600 inside a 0700 secrets directory", () => {
    setPassphrase("correct horse battery staple");
    expect(modeOf(paths.secret("passphrase"))).toBe(0o600);
    expect(modeOf(paths.secretsDir())).toBe(0o700);
  });

  it("salts per install, so the same passphrase hashes differently", () => {
    setPassphrase("correct horse battery staple");
    const first = JSON.parse(fs.readFileSync(paths.secret("passphrase"), "utf8"));
    setPassphrase("correct horse battery staple");
    const second = JSON.parse(fs.readFileSync(paths.secret("passphrase"), "utf8"));
    expect(second.salt).not.toBe(first.salt);
    expect(second.hash).not.toBe(first.hash);
    expect(verifyPassphrase("correct horse battery staple")).toBe(true);
  });

  it("normalises unicode so an equivalent passphrase still works", () => {
    // Same string, composed versus decomposed. The accent is the point: a
    // passphrase typed on one keyboard layout and again on another has to
    // hash the same, and only a non-ASCII character can prove that.
    setPassphrase("café litteraire ouvert");
    expect(verifyPassphrase("café litteraire ouvert")).toBe(true);
  });
});

describe("sessions", () => {
  it("issues a token that verifies", () => {
    const { token } = issueSession();
    const payload = verifySession(token);
    expect(payload?.sub).toBe("operator");
    expect(payload?.exp).toBeGreaterThan(payload?.iat ?? 0);
  });

  it("issues a distinct CSRF token alongside the session", () => {
    const a = issueSession();
    const b = issueSession();
    expect(a.csrf).toMatch(/^[0-9a-f]{64}$/);
    expect(a.csrf).not.toBe(b.csrf);
  });

  it("refuses a tampered payload", () => {
    const { token } = issueSession();
    const [body, signature] = token.split(".") as [string, string];
    const forged = Buffer.from(
      JSON.stringify({ sub: "attacker", iat: 0, exp: 9_999_999_999 }),
    )
      .toString("base64")
      .replace(/\+/g, "-")
      .replace(/\//g, "_")
      .replace(/=+$/, "");
    expect(verifySession(`${forged}.${signature}`)).toBeNull();
    expect(verifySession(`${body}.${signature}x`)).toBeNull();
  });

  it("refuses a malformed or absent token", () => {
    expect(verifySession(undefined)).toBeNull();
    expect(verifySession("")).toBeNull();
    expect(verifySession("not-a-token")).toBeNull();
    expect(verifySession("a.b.c")).toBeNull();
  });

  it("refuses an expired token", () => {
    const now = new Date("2026-09-11T00:00:00Z");
    const { token } = issueSession(now);
    const later = new Date(now.getTime() + (SESSION_TTL_SECONDS + 1) * 1000);
    expect(verifySession(token, later)).toBeNull();
    expect(
      verifySession(token, new Date(now.getTime() + 60_000)),
    ).not.toBeNull();
  });

  it("refuses a token signed with a different secret", () => {
    const { token } = issueSession();
    // Replace the session secret, as a fresh install would.
    fs.writeFileSync(paths.secret("session-secret"), "0".repeat(64));
    expect(verifySession(token)).toBeNull();
  });
});

describe("CSRF double submit", () => {
  it("matches only when cookie and header are identical", () => {
    const { csrf } = issueSession();
    expect(csrfMatches(csrf, csrf)).toBe(true);
    expect(csrfMatches(csrf, `${csrf}x`)).toBe(false);
    expect(csrfMatches(csrf, csrf.slice(0, -1))).toBe(false);
    // Flip the last character to something it is definitely not. Replacing it
    // with a fixed "0" made this test pass about fifteen times in sixteen: one
    // random token in sixteen already ends in a zero, and then the "changed"
    // token was the original.
    const flipped = `${csrf.slice(0, -1)}${csrf.endsWith("0") ? "1" : "0"}`;
    expect(flipped).not.toBe(csrf);
    expect(csrfMatches(csrf, flipped)).toBe(false);
  });

  it("fails closed when either side is missing", () => {
    const { csrf } = issueSession();
    expect(csrfMatches(undefined, csrf)).toBe(false);
    expect(csrfMatches(csrf, null)).toBe(false);
    expect(csrfMatches(undefined, undefined)).toBe(false);
    expect(csrfMatches("", "")).toBe(false);
  });

  it("uses a header name the browser cannot set cross-origin without CORS", () => {
    expect(CSRF_HEADER).toBe("x-csrf-token");
  });
});

describe("login rate limit", () => {
  it("allows the first five attempts", () => {
    for (let i = 0; i < LOGIN_ATTEMPT_LIMIT; i += 1) {
      expect(checkLoginRate("client").allowed).toBe(true);
      recordFailedLogin("client");
    }
    const blocked = checkLoginRate("client");
    expect(blocked.allowed).toBe(false);
    expect(blocked.retryAfterSeconds).toBeGreaterThan(0);
  });

  it("only counts failures, so a working passphrase is never limited", () => {
    for (let i = 0; i < 20; i += 1) {
      expect(checkLoginRate("client").allowed).toBe(true);
    }
  });

  it("forgets attempts once the window passes", () => {
    const start = 1_000_000;
    for (let i = 0; i < LOGIN_ATTEMPT_LIMIT; i += 1) {
      recordFailedLogin("client", start);
    }
    expect(checkLoginRate("client", start + 1_000).allowed).toBe(false);
    expect(checkLoginRate("client", start + 61_000).allowed).toBe(true);
  });

  it("tracks clients separately", () => {
    for (let i = 0; i < LOGIN_ATTEMPT_LIMIT; i += 1) {
      recordFailedLogin("a");
    }
    expect(checkLoginRate("a").allowed).toBe(false);
    expect(checkLoginRate("b").allowed).toBe(true);
  });
});

describe("bind guard", () => {
  it("always allows loopback", () => {
    for (const host of ["127.0.0.1", "127.0.0.53", "::1", "localhost"]) {
      expect(isLoopbackHost(host), host).toBe(true);
      expect(
        bindAllowed({ hostname: host, passphraseSet: false, allowLanFlag: undefined })
          .allowed,
      ).toBe(true);
    }
  });

  it("refuses a LAN bind without the explicit flag", () => {
    const verdict = bindAllowed({
      hostname: "192.168.0.5",
      passphraseSet: true,
      allowLanFlag: undefined,
    });
    expect(verdict.allowed).toBe(false);
    expect(verdict.reason).toMatch(/NOTE4C_TOWER_ALLOW_LAN=1/);
  });

  it("refuses a LAN bind without a passphrase, even with the flag", () => {
    const verdict = bindAllowed({
      hostname: "192.168.0.5",
      passphraseSet: false,
      allowLanFlag: "1",
    });
    expect(verdict.allowed).toBe(false);
    expect(verdict.reason).toMatch(/passphrase/);
  });

  it("refuses binding every interface even when both conditions are met", () => {
    for (const host of ["0.0.0.0", "::"]) {
      const verdict = bindAllowed({
        hostname: host,
        passphraseSet: true,
        allowLanFlag: "1",
      });
      expect(verdict.allowed, host).toBe(false);
      expect(verdict.reason).toMatch(/every interface/);
    }
  });

  it("allows a named LAN address when both conditions are met", () => {
    expect(
      bindAllowed({
        hostname: "192.168.0.5",
        passphraseSet: true,
        allowLanFlag: "1",
      }).allowed,
    ).toBe(true);
  });

  it("throws with the reason when asserted", () => {
    expect(() =>
      assertBindAllowed({
        hostname: "192.168.0.5",
        passphraseSet: true,
        allowLanFlag: "0",
      }),
    ).toThrow(BindRefusedError);
  });
});
