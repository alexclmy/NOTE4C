import { NextResponse } from "next/server";
import { z } from "zod";
import { REAL_MODE_CONFIRMATION } from "@/core/gates";
import { bridgeTokenPath } from "@/server/config";
import { appendAudit } from "@/server/audit";
import { guarded, jsonError } from "@/server/auth/guard";
import { DeviceClient } from "@/server/device/client";
import { isRfc1918 } from "@/server/device/address";
import {
  bridgeTokenAvailable,
  deviceTokenIsSet,
  forgetDeviceToken,
  importBridgeToken,
} from "@/server/device/token";
import { blockingPush } from "@/server/device/ledger";
import { invalidateOverviewSnapshot } from "@/server/overview/snapshot";
import { readState, updateState } from "@/server/store/state";

export const dynamic = "force-dynamic";

/**
 * The real-device path. OFF by default and gated at every step.
 *
 * Switching to "real" needs a typed confirmation AND a successful read-only
 * status read, because a mode switch that cannot even read the device is a
 * promise the tower cannot keep. Importing the composer bridge's token needs
 * its own explicit consent naming the source path, and the value is never
 * displayed, logged or returned.
 *
 * The operator-facing procedure is the second half of QUICKSTART.md.
 */

const BodySchema = z.discriminatedUnion("action", [
  z.object({
    action: z.literal("import-bridge-token"),
    /** Must equal the path shown in the consent dialog. */
    consentPath: z.string(),
  }),
  z.object({ action: z.literal("forget-token") }),
  z.object({
    action: z.literal("set-mode"),
    mode: z.enum(["mock", "real"]),
    /** Required when switching to real. */
    confirm: z.string().optional(),
  }),
]);

/**
 * Anything on this route changes which device the tower is talking to, or
 * whether it can talk to it at all. The Overview snapshot is a cached reading
 * of the previous answer to that question, so it is dropped on the way out
 * rather than left to age into a reading of the wrong device.
 */
export const POST = guarded(
  { mutating: true, schema: BodySchema, action: "device.mode" },
  async ({ body }) => {
    // Unconditionally, and before any branching. A refused request costs one
    // extra rebuild later and nothing else; working out which of four exit
    // paths changed the device would be four chances to miss one.
    invalidateOverviewSnapshot();

    if (body.action === "import-bridge-token") {
      const consentTarget = bridgeTokenPath();
      if (consentTarget.length === 0 || body.consentPath !== consentTarget) {
        return jsonError(
          400,
          "consent_mismatch",
          "The consent path does not match the token the tower would read",
        );
      }
      if (!bridgeTokenAvailable()) {
        return jsonError(
          404,
          "no_bridge_token",
          `No device token found at ${consentTarget}`,
        );
      }
      importBridgeToken();
      appendAudit({
        action: "device.token.import",
        target: consentTarget,
        // The path is recorded. The value never is.
        params: { source: consentTarget, token: "supplied" },
        outcome: "ok",
        deviceConfirmed: null,
        detail:
          "Copied the composer bridge credential into the tower secrets directory at 0600. Re-pairing instead would invalidate the bridge's token.",
      });
      return NextResponse.json({ deviceTokenSet: deviceTokenIsSet() });
    }

    if (body.action === "forget-token") {
      forgetDeviceToken();
      appendAudit({
        action: "device.token.forget",
        target: "tower",
        outcome: "ok",
        deviceConfirmed: null,
        detail: "Removed the tower's copy of the device token",
      });
      return NextResponse.json({ deviceTokenSet: deviceTokenIsSet() });
    }

    // set-mode
    if (body.mode === "mock") {
      updateState({ deviceMode: "mock" });
      appendAudit({
        action: "device.mode",
        target: "tower",
        params: { mode: "mock" },
        outcome: "ok",
        deviceConfirmed: null,
        detail: "Back to the simulated device",
      });
      return NextResponse.json({ deviceMode: "mock", simulated: true });
    }

    if (body.confirm !== REAL_MODE_CONFIRMATION) {
      return jsonError(
        400,
        "confirmation_required",
        `Type ${REAL_MODE_CONFIRMATION} to switch the tower to the real panel`,
      );
    }

    const blocking = blockingPush();
    if (blocking) {
      return jsonError(
        409,
        "unresolved_push",
        `A ${blocking.state} push must be resolved before changing device mode`,
      );
    }

    const state = readState();
    if (!isRfc1918(state.deviceAddress)) {
      return jsonError(
        400,
        "bad_address",
        "Set a private IPv4 device address before switching to the real panel",
      );
    }
    if (!deviceTokenIsSet()) {
      return jsonError(
        400,
        "no_token",
        "Import or pair a device token before switching to the real panel",
      );
    }

    // The one permitted read-only call, and it is still user initiated.
    const probe = new DeviceClient({
      mode: "real",
      address: state.deviceAddress,
      token: null,
    });

    try {
      const status = await probe.status();
      updateState({ deviceMode: "real" });
      appendAudit({
        action: "device.mode",
        target: state.deviceAddress,
        params: { mode: "real", api: status.api },
        outcome: "ok",
        deviceConfirmed: true,
        detail: `Confirmed by a read-only status read: firmware ${status.firmware}, api ${status.api}`,
      });
      return NextResponse.json({
        deviceMode: "real",
        simulated: false,
        status,
      });
    } catch (error) {
      const detail =
        error instanceof Error ? error.message : "The device could not be reached";
      appendAudit({
        action: "device.mode",
        target: state.deviceAddress,
        params: { mode: "real" },
        outcome: "refused",
        deviceConfirmed: false,
        detail: `Refused the switch: ${detail}`,
      });
      return jsonError(
        502,
        "unreachable",
        `The tower stayed on the mock device: ${detail}`,
      );
    }
  },
);
