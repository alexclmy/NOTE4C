import { randomUUID } from "node:crypto";
import { NextResponse } from "next/server";
import { z } from "zod";
import { appendAudit } from "@/server/audit";
import { guarded, jsonError } from "@/server/auth/guard";
import { deviceContext } from "@/server/device/context";
import { entryByKey, isLive, negotiate } from "@/core/registry";

export const dynamic = "force-dynamic";

const BodySchema = z.object({
  action: z.enum(["restart", "sleep"]),
  /** The word the user typed. Compared against the registry's confirmGate. */
  confirm: z.string(),
  /**
   * Supplied by the browser and reused across a retry of the same intent, so
   * a user who clicks twice, or a request whose answer was lost, does not
   * reboot the device twice. A key minted here per request would defeat that.
   */
  idempotencyKey: z.string().min(8).max(64).optional(),
});

/**
 * Restart and sleep.
 *
 * Both are gated three times over and that is proportionate: a restart at the
 * wrong moment interrupts a panel refresh, and a sleep takes the device off
 * the network with only the physical BOOT button to bring it back. The tower
 * cannot undo either one.
 */
export const POST = guarded(
  { mutating: true, schema: BodySchema, action: "device.action" },
  async ({ body }) => {
    const entry = entryByKey(`system.${body.action}`);
    if (!entry) {
      return jsonError(400, "unknown_action", "No such device action");
    }

    const { client, simulated } = await deviceContext();

    let status;
    try {
      status = await client.status();
    } catch (error) {
      return jsonError(
        502,
        "unreachable",
        error instanceof Error ? error.message : "The device could not be reached",
      );
    }

    const device = negotiate(status);
    if (!isLive(entry, device)) {
      return jsonError(
        400,
        "not_supported",
        `This device does not report the ${entry.capability} capability`,
      );
    }

    if (body.confirm !== entry.confirmGate) {
      appendAudit({
        action: `device.action.${body.action}`,
        target: simulated ? "mock device" : "device",
        outcome: "refused",
        deviceConfirmed: false,
        detail: "The confirmation word did not match",
      });
      return jsonError(
        400,
        "confirmation_required",
        `Type ${entry.confirmGate} to confirm`,
      );
    }

    const idempotencyKey = body.idempotencyKey ?? randomUUID();

    try {
      const result = await client.runAction(body.action, idempotencyKey);
      appendAudit({
        action: `device.action.${body.action}`,
        target: simulated ? "mock device" : "device",
        params: { idempotencyKey, replay: result.replay },
        outcome: "ok",
        deviceConfirmed: true,
        detail: result.replay
          ? "The device had already accepted this request and did nothing further"
          : `The device accepted it and acts in ${result.atMs} ms`,
      });
      return NextResponse.json({
        action: result.action,
        scheduled: result.scheduled,
        replay: result.replay,
        atMs: result.atMs,
        idempotencyKey,
      });
    } catch (error) {
      const detail =
        error instanceof Error ? error.message : "The device refused the action";
      appendAudit({
        action: `device.action.${body.action}`,
        target: simulated ? "mock device" : "device",
        params: { idempotencyKey },
        outcome: "failed",
        deviceConfirmed: false,
        detail,
      });
      return jsonError(502, "device_refused", detail);
    }
  },
);
