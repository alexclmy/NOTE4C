import { NextResponse } from "next/server";
import { z } from "zod";
import { appendAudit } from "@/server/audit";
import { guarded, jsonError } from "@/server/auth/guard";
import { deviceContext } from "@/server/device/context";
import {
  describeDeviceFailure,
  type DeviceFailure,
} from "@/server/device/failure";
import {
  acknowledge,
  deliverQueuedPush,
  recheck,
} from "@/server/device/pushPipeline";
import { findPush } from "@/server/device/ledger";
import { noteDeviceSeen } from "@/server/device/powerIntent";
import { invalidateOverviewSnapshot } from "@/server/overview/snapshot";

export const dynamic = "force-dynamic";

function pushIdFrom(pathname: string): string {
  const parts = pathname.split("/").filter(Boolean);
  return parts[parts.length - 1] ?? "";
}

const BodySchema = z.object({
  action: z.enum(["recheck", "acknowledge", "deliver", "cancel"]),
  note: z.string().max(200).default(""),
});

/**
 * The ways an unresolved push leaves the blocking set.
 *
 * Two of them are about an unknown outcome. "recheck" asks the device what it
 * is actually showing, which is the only path to verified without a new write.
 * "acknowledge" is a human accepting an unknown outcome, and is recorded as
 * exactly that.
 *
 * Two are about a queued one, which is not an unknown at all — nothing was
 * sent, and the tower knows precisely what it is holding. "deliver" is the
 * explicit "try it now" for somebody who has just pressed the button on the
 * device and does not want to wait for the next scheduler pass. "cancel"
 * withdraws it. Both exist as mutations on this route, behind the session and
 * the CSRF check, for the same reason the power route's reconcile does:
 * reading a page must never be what sends a frame to a panel.
 */
export const POST = guarded(
  { mutating: true, schema: BodySchema, action: "device.push.resolve" },
  async ({ request, body }) => {
    const pushId = pushIdFrom(request.nextUrl.pathname);
    const record = findPush(pushId);
    if (!record) {
      return jsonError(404, "not_found", "No such push in the ledger");
    }

    if (body.action === "acknowledge" || body.action === "cancel") {
      // Cancelling is only meaningful for a push that was never sent. Asking
      // to cancel one that is on the wire would be asking the tower to forget
      // something the device may well be acting on, which is precisely the
      // pretence `uncertain` exists to prevent.
      if (body.action === "cancel" && record.state !== "queued") {
        return jsonError(
          409,
          "not_queued",
          `This push is ${record.state}, not queued. Something went out, so it cannot be withdrawn — re-check it, or acknowledge the unknown outcome.`,
        );
      }
      acknowledge(pushId, body.note);
      invalidateOverviewSnapshot();
      return NextResponse.json({
        state: "acknowledged",
        detail:
          record.state === "queued"
            ? "Withdrawn. The frame was never sent, so nothing on the device changed, and the tower is no longer holding it."
            : "Recorded as an accepted unknown outcome",
      });
    }

    if (body.action === "deliver") {
      if (record.state !== "queued") {
        return jsonError(
          409,
          "not_queued",
          `This push is ${record.state}, so there is nothing held here to send.`,
        );
      }

      const { client, simulated } = await deviceContext();
      // One read, and it answers the only question that matters here: is the
      // device actually there. It is then handed to the delivery so that the
      // whole action costs a single status read plus, at most, one write.
      let status = null;
      let reachable = false;
      let failure: DeviceFailure | null = null;
      try {
        status = await client.status();
        reachable = true;
        noteDeviceSeen(new Date(), status);
      } catch (error) {
        // This used to be `catch {}`, and that bare catch is most of why a
        // production outage was invisible: the panel was answering in 70 ms,
        // the transport was failing inside this process, and the only thing
        // anyone could see was a frame that kept queueing itself.
        //
        // A device that is away is still not an event — `notable` is false and
        // nothing is written, exactly as before. A fault is recorded once, on
        // a button somebody pressed, so it cannot be a hundred lines an hour.
        failure = describeDeviceFailure(error);
        if (failure.notable) {
          appendAudit({
            action: "device.status.read",
            target: pushId,
            params: { kind: failure.kind, code: failure.code, errno: failure.errno },
            outcome: "failed",
            deviceConfirmed: false,
            detail: `Could not read the device status before delivering: ${failure.detail}`,
          });
        }
      }

      const result = await deliverQueuedPush({
        client,
        status,
        reachable,
        deviceMode: simulated ? "mock" : "real",
      });
      invalidateOverviewSnapshot();
      return NextResponse.json({
        state: result.outcome,
        detail: result.detail,
        attempted: result.attempted,
        reachable,
        // Why the device did not answer, when it did not. `null` when it did.
        // A reader looking at "waiting" can now tell a sleeping panel from a
        // transport that never left this machine.
        statusFailure: failure,
      });
    }

    const { client } = await deviceContext();
    return NextResponse.json(await recheck(pushId, client));
  },
);
