import { NextResponse } from "next/server";
import { z } from "zod";
import { appendAudit } from "@/server/audit";
import { guarded, jsonError } from "@/server/auth/guard";
import { deviceContext } from "@/server/device/context";
import {
  DeviceRevisionConflict,
  type ConfigPatchValues,
} from "@/server/device/client";
import {
  SETTINGS_REGISTRY,
  isEditable,
  needsConfirmation,
  negotiate,
} from "@/core/registry";

export const dynamic = "force-dynamic";

/**
 * Read the device configuration.
 *
 * Capability is negotiated from the device's own status before the config
 * route is called at all. A device that does not advertise config.v2 gets an
 * honest "the firmware does not expose this" rather than a 404 the UI would
 * have to guess the meaning of.
 */
export const GET = guarded({}, async () => {
  const { client, simulated } = await deviceContext();

  let status;
  try {
    status = await client.status();
  } catch (error) {
    return NextResponse.json({
      reachable: false,
      simulated,
      supported: false,
      detail:
        error instanceof Error ? error.message : "The device could not be reached",
    });
  }

  const device = negotiate(status);
  if (device.api < 2 || !device.capabilities.includes("config.v2")) {
    return NextResponse.json({
      reachable: true,
      simulated,
      supported: false,
      device,
      detail:
        "This firmware does not expose a configuration API. Every setting stays on-device only.",
    });
  }

  try {
    const config = await client.getConfig();
    return NextResponse.json({
      reachable: true,
      simulated,
      supported: true,
      device,
      revision: config.revision,
      config: config.config,
      readAt: new Date().toISOString(),
    });
  } catch (error) {
    return NextResponse.json({
      reachable: true,
      simulated,
      supported: false,
      device,
      detail: error instanceof Error ? error.message : "The device refused the read",
    });
  }
});

/**
 * One Apply action writes every edited field at once.
 *
 * Batched deliberately. Each write costs a round trip on a device that serves
 * four sockets and runs its own UI through one of them, and each write bumps
 * the revision, so writing six fields one at a time would mean five of them
 * racing the tower's own previous write.
 */
const PatchSchema = z.object({
  expectedRevision: z.number().int().nonnegative(),
  set: z.record(
    z.string(),
    z.union([z.number(), z.boolean(), z.string()]),
  ),
  /** The word the user typed, for fields that require one. */
  confirm: z.string().optional(),
});

export const PATCH = guarded(
  { mutating: true, schema: PatchSchema, action: "device.config.write" },
  async ({ body }) => {
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

    // Validate against the registry before anything leaves this process. The
    // firmware checks all of this again, and should; doing it here as well is
    // what lets the UI name the problem without spending a round trip on it.
    const entries = Object.entries(body.set);
    if (entries.length === 0) {
      return jsonError(400, "empty_patch", "No fields were changed");
    }

    let deviceConfirm: string | undefined;
    for (const [field, value] of entries) {
      const entry = SETTINGS_REGISTRY.find((row) => row.field === field);
      if (!entry) {
        return jsonError(400, "unknown_field", `"${field}" is not a device setting`);
      }
      if (!isEditable(entry, device)) {
        return jsonError(
          400,
          "not_writable",
          `This device does not report the capability needed to write ${entry.label}`,
        );
      }
      if (entry.oneWayTo !== undefined && value !== entry.oneWayTo) {
        return jsonError(
          400,
          "one_way",
          `${entry.label} can only be set one way from here. Changing it back takes a button press on the device.`,
        );
      }
      if (entry.constraints?.values && !entry.constraints.values.includes(value as never)) {
        return jsonError(
          400,
          "out_of_range",
          `${entry.label} accepts only ${entry.constraints.values.join(", ")}`,
        );
      }
      if (typeof value === "number" && entry.constraints) {
        const { min, max } = entry.constraints;
        if ((min !== undefined && value < min) || (max !== undefined && value > max)) {
          return jsonError(
            400,
            "out_of_range",
            `${entry.label} must be between ${min} and ${max}`,
          );
        }
      }
      if (needsConfirmation(entry, value)) {
        if (body.confirm !== entry.confirmGate) {
          return jsonError(
            400,
            "confirmation_required",
            `Type ${entry.confirmGate} to confirm the change to ${entry.label}`,
          );
        }
        deviceConfirm = entry.deviceConfirm;
      }
    }

    try {
      const result = await client.patchConfig(
        body.expectedRevision,
        body.set as ConfigPatchValues,
        deviceConfirm,
      );
      appendAudit({
        action: "device.config.write",
        target: simulated ? "mock device" : "device",
        params: { fields: Object.keys(body.set), expectedRevision: body.expectedRevision },
        outcome: "ok",
        deviceConfirmed: true,
        detail: `Revision ${body.expectedRevision} to ${result.revision}`,
      });
      return NextResponse.json({
        revision: result.revision,
        config: result.config,
        applied: result.applied ?? {},
      });
    } catch (error) {
      if (error instanceof DeviceRevisionConflict) {
        // Not retried, and never retried automatically. The user has to see
        // what changed underneath them before deciding whether they still
        // want their edit.
        appendAudit({
          action: "device.config.write",
          target: simulated ? "mock device" : "device",
          params: { fields: Object.keys(body.set), expectedRevision: body.expectedRevision },
          outcome: "refused",
          deviceConfirmed: false,
          detail: `Revision conflict: the device is at ${error.deviceRevision ?? "an unknown revision"}`,
        });
        return jsonError(
          409,
          "revision_conflict",
          "The device configuration changed since it was read. Nothing was written.",
          { deviceRevision: error.deviceRevision },
        );
      }
      const detail =
        error instanceof Error ? error.message : "The device refused the write";
      appendAudit({
        action: "device.config.write",
        target: simulated ? "mock device" : "device",
        params: { fields: Object.keys(body.set), expectedRevision: body.expectedRevision },
        outcome: "failed",
        deviceConfirmed: false,
        detail,
      });
      return jsonError(502, "device_refused", detail);
    }
  },
);
