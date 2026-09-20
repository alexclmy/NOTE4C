import { NextResponse } from "next/server";
import { z } from "zod";
import { appendAudit } from "@/server/audit";
import { guarded, jsonError } from "@/server/auth/guard";
import { deviceContext } from "@/server/device/context";
import { isRfc1918, isLoopback } from "@/server/device/address";

export const dynamic = "force-dynamic";

const BodySchema = z.object({
  url: z.string().min(1).max(200),
  token: z.string().min(1).max(200),
});

/**
 * The one remotely writable setting at api 1.
 *
 * Write only in both directions: the device never echoes the token back, this
 * route never returns it, and the audit entry records only that a value was
 * supplied. The hub URL is checked to be on the LAN, because the device sends
 * audio to it as plaintext HTTP.
 */
export const POST = guarded(
  { mutating: true, schema: BodySchema, action: "voice.hub.configure" },
  async ({ body }) => {
    let url: URL;
    try {
      url = new URL(body.url);
    } catch {
      return jsonError(400, "bad_url", "The hub URL is not a valid URL");
    }

    if (url.protocol !== "http:" && url.protocol !== "https:") {
      return jsonError(400, "bad_url", "The hub URL must be http or https");
    }
    if (!isRfc1918(url.hostname) && !isLoopback(url.hostname)) {
      return jsonError(
        400,
        "bad_url",
        "The hub URL must point at a private LAN address. The device uploads audio to it in the clear.",
      );
    }

    const { client, simulated } = await deviceContext();

    try {
      const result = await client.setVoiceHub(body.url, body.token);
      // `tokenSupplied`, not `token`. The redaction in `appendAudit` would
      // have replaced the value, and that is still the wrong place to rely on:
      // the secret would exist as a live argument inside the serialiser, one
      // edit to the key pattern away from reaching a file. A boolean records
      // the only thing an auditor needs — that a token was written — and
      // cannot be un-redacted by a refactor.
      appendAudit({
        action: "voice.hub.configure",
        target: simulated ? "mock device" : "device",
        params: { url: body.url, tokenSupplied: body.token.length > 0 },
        outcome: "ok",
        deviceConfirmed: result.accepted,
        detail: "Hub URL and token written. Configuring the hub does not unmute.",
      });
      return NextResponse.json({
        accepted: result.accepted,
        url: result.url,
        tokenSet: result.tokenSet,
        plaintextWarning: url.protocol === "http:",
      });
    } catch (error) {
      const detail =
        error instanceof Error ? error.message : "The device refused the write";
      appendAudit({
        action: "voice.hub.configure",
        target: simulated ? "mock device" : "device",
        params: { url: body.url, tokenSupplied: body.token.length > 0 },
        outcome: "failed",
        deviceConfirmed: false,
        detail,
      });
      return jsonError(502, "device_refused", detail);
    }
  },
);
