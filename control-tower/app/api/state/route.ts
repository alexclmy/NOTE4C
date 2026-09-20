import { NextResponse } from "next/server";
import { z } from "zod";
import { appendAudit } from "@/server/audit";
import { guarded, jsonError } from "@/server/auth/guard";
import { isRfc1918 } from "@/server/device/address";
import { bridgeTokenPath } from "@/server/config";
import { deviceTokenIsSet, bridgeTokenAvailable } from "@/server/device/token";
import { readState, updateState } from "@/server/store/state";

export const dynamic = "force-dynamic";

function publicState() {
  const state = readState();
  return {
    selectedDashboardId: state.selectedDashboardId,
    deviceMode: state.deviceMode,
    deviceAddress: state.deviceAddress,
    firstRunCompletedAt: state.firstRunCompletedAt,
    density: state.density,
    lastAutoRefreshAt: state.lastAutoRefreshAt,
    lastAutoRefreshDashboardId: state.lastAutoRefreshDashboardId,
    lastAutoRefreshOutcome: state.lastAutoRefreshOutcome,
    // Booleans only. A token's presence is useful; its value never is.
    deviceTokenSet: deviceTokenIsSet(),
    bridgeTokenAvailable: bridgeTokenAvailable(),
    /**
     * The *path* the import would read, never its contents.
     *
     * The consent dialog has to name the exact file it is about to copy —
     * consent to "a token somewhere" is not consent — and the path is
     * configuration on this machine rather than a secret. Empty when
     * NOTE4C_BRIDGE_TOKEN_PATH is unset, which hides the button entirely.
     */
    bridgeTokenPath: bridgeTokenPath(),
  };
}

export const GET = guarded({}, () => NextResponse.json(publicState()));

const PatchSchema = z.object({
  selectedDashboardId: z.string().max(64).nullable().optional(),
  deviceAddress: z.string().max(45).optional(),
  density: z.enum(["comfortable", "compact"]).optional(),
  firstRunCompleted: z.literal(true).optional(),
});

export const PATCH = guarded(
  { mutating: true, schema: PatchSchema, action: "state.update" },
  async ({ body }) => {
    const patch: Record<string, unknown> = {};

    if (body.deviceAddress !== undefined) {
      const address = body.deviceAddress.trim();
      if (!isRfc1918(address)) {
        return jsonError(
          400,
          "bad_address",
          "The device address must be a private IPv4 address (10/8, 172.16/12 or 192.168/16)",
        );
      }
      patch.deviceAddress = address;
    }

    if (body.selectedDashboardId !== undefined) {
      patch.selectedDashboardId = body.selectedDashboardId;
    }
    if (body.density !== undefined) patch.density = body.density;
    if (body.firstRunCompleted) {
      patch.firstRunCompletedAt = new Date().toISOString();
    }

    updateState(patch);
    appendAudit({
      action: "state.update",
      target: "tower",
      params: patch,
      outcome: "ok",
      detail: `Updated ${Object.keys(patch).join(", ") || "nothing"}`,
    });
    return NextResponse.json(publicState());
  },
);
