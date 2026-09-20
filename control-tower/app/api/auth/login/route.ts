import { NextResponse } from "next/server";
import type { NextRequest } from "next/server";
import { z } from "zod";
import { appendAudit } from "@/server/audit";
import {
  attachSession,
  clientKey,
  requestIsSecure,
} from "@/server/auth/cookies";
import { jsonError, publicRoute } from "@/server/auth/guard";
import { passphraseIsSet, verifyPassphrase } from "@/server/auth/passphrase";
import { checkLoginRate, recordFailedLogin } from "@/server/auth/rateLimit";

export const dynamic = "force-dynamic";

const BodySchema = z.object({ passphrase: z.string().max(256) });

export const POST = publicRoute(async (request: NextRequest) => {
  if (!passphraseIsSet()) {
    return jsonError(
      409,
      "not_configured",
      "No tower passphrase is set yet. Complete the first run instead.",
    );
  }

  const key = clientKey(request.headers);
  const verdict = checkLoginRate(key);
  if (!verdict.allowed) {
    appendAudit({
      action: "auth.login",
      target: "tower",
      outcome: "refused",
      detail: "Rate limited",
    });
    return jsonError(429, "rate_limited", "Too many attempts. Wait a moment.", {
      retryAfterSeconds: verdict.retryAfterSeconds,
    });
  }

  let raw: unknown;
  try {
    raw = await request.json();
  } catch {
    return jsonError(400, "bad_body", "The request body is not valid JSON");
  }

  const parsed = BodySchema.safeParse(raw);
  // A wrong passphrase and a malformed body get the same answer: a login route
  // that distinguishes them tells an attacker which half they got right.
  if (!parsed.success || !verifyPassphrase(parsed.data.passphrase)) {
    recordFailedLogin(key);
    appendAudit({
      action: "auth.login",
      target: "tower",
      outcome: "refused",
      detail: "Incorrect passphrase",
    });
    return jsonError(401, "invalid_passphrase", "That passphrase is not correct");
  }

  appendAudit({
    action: "auth.login",
    target: "tower",
    outcome: "ok",
    detail: "Signed in",
  });

  return attachSession(
    NextResponse.json({ ok: true }),
    requestIsSecure(request.url),
  );
});
