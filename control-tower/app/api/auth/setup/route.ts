import { NextResponse } from "next/server";
import type { NextRequest } from "next/server";
import { z } from "zod";
import { appendAudit } from "@/server/audit";
import { attachSession, requestIsSecure } from "@/server/auth/cookies";
import { jsonError, publicRoute } from "@/server/auth/guard";
import {
  MIN_PASSPHRASE_LENGTH,
  passphraseIsSet,
  setPassphrase,
} from "@/server/auth/passphrase";

export const dynamic = "force-dynamic";

const BodySchema = z.object({
  passphrase: z.string().min(MIN_PASSPHRASE_LENGTH).max(256),
});

/**
 * First run only. Once a passphrase exists this route refuses, so it can never
 * be used to overwrite one from an unauthenticated request.
 */
export const POST = publicRoute(async (request: NextRequest) => {
  if (passphraseIsSet()) {
    return jsonError(
      409,
      "already_configured",
      "A tower passphrase is already set. Sign in instead.",
    );
  }

  let raw: unknown;
  try {
    raw = await request.json();
  } catch {
    return jsonError(400, "bad_body", "The request body is not valid JSON");
  }

  const parsed = BodySchema.safeParse(raw);
  if (!parsed.success) {
    return jsonError(
      400,
      "weak_passphrase",
      `The passphrase must be at least ${MIN_PASSPHRASE_LENGTH} characters`,
    );
  }

  setPassphrase(parsed.data.passphrase);
  // No params, and specifically not the passphrase.
  //
  // `redactParams` would have caught it — the key is named `passphrase` and
  // the pattern matches — but that is a net under a tightrope nobody needed to
  // walk. Handing the plaintext to the audit writer means it exists as a live
  // value inside a function whose job is to serialise its argument, one
  // refactor of the redaction pattern away from being written to a file on
  // disk, and visible in a heap dump or a stack trace taken in between. The
  // entry is just as useful without it: that a passphrase was set on first run
  // is the fact worth recording, and the value is not part of that fact.
  appendAudit({
    action: "auth.setup",
    target: "tower",
    outcome: "ok",
    detail: "Tower passphrase set on first run",
  });

  return attachSession(
    NextResponse.json({ ok: true }),
    requestIsSecure(request.url),
  );
});
