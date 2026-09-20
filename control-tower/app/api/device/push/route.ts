import { NextResponse } from "next/server";
import { z } from "zod";
import { appendAudit } from "@/server/audit";
import { guarded, jsonError } from "@/server/auth/guard";
import { deviceContext } from "@/server/device/context";
import {
  REAL_PUSH_CONFIRMATION,
  prepareFrame,
  push,
} from "@/server/device/pushPipeline";
import { blockingPush, lastVerifiedPush, readPushes } from "@/server/device/ledger";
import { invalidateOverviewSnapshot } from "@/server/overview/snapshot";
import { collectSources } from "@/server/sources";
import { readRecord, readVersion } from "@/server/store/dashboards";
import { readState } from "@/server/store/state";

export const dynamic = "force-dynamic";

/**
 * What the ledger currently says about sending — and nothing else.
 *
 * This route reads two local files and stops. It opens no socket at the
 * device, starts no poll, and cannot change anything: it adds no power to the
 * tower that the POST below did not already have.
 *
 * It exists because the POST is *blocking*. A push holds its request open
 * while it polls the device for a verdict, for up to `WORST_CASE_ACK_MS`, and
 * a person watching a modal for six minutes with no indication of which part
 * is happening will reasonably conclude it has hung. The send dialog polls
 * this while the POST is in flight and lights its stages from the transitions
 * the pipeline has actually written down — `pending`, then `sent`, then the
 * verdict — so every dot on that screen is a line in an append-only file
 * rather than a timer pretending to be progress.
 *
 * That distinction is the whole reason this is a separate route instead of
 * the dialog animating on a schedule: a cosmetic progress bar for an operation
 * that reaches physical hardware is the exact species of lie this product is
 * built not to tell.
 */
export const GET = guarded({}, async () => {
  return NextResponse.json({
    blocking: blockingPush(),
    lastPush: readPushes()[0] ?? null,
  });
});

const BodySchema = z.object({
  dashboardId: z.string().max(64).optional(),
  version: z.number().int().positive().optional(),
  force: z.boolean().default(false),
  /** Required when deviceMode is real. */
  confirm: z.string().max(32).optional(),
  /** Answer without polling. Used by the dry-run preflight in the UI. */
  dryRun: z.boolean().default(false),
});

export const POST = guarded(
  { mutating: true, schema: BodySchema, action: "device.push" },
  async ({ body }) => {
    const state = readState();
    const dashboardId = body.dashboardId ?? state.selectedDashboardId;
    if (!dashboardId) {
      return jsonError(
        400,
        "no_selection",
        "No dashboard is selected for the next push",
      );
    }

    const record = readRecord(dashboardId);
    if (!record) {
      return jsonError(404, "not_found", `No dashboard with id "${dashboardId}"`);
    }

    const version = body.version ?? record.latestVersion;
    const doc = body.version ? readVersion(dashboardId, body.version).doc : record.doc;

    const { client, simulated } = await deviceContext();

    // Real pushes need a typed confirmation word. Mock pushes do not: nothing
    // physical happens and the banner already says so.
    //
    // A dry run needs no word either, and the exemption is not a loosening.
    // `dryRun` renders the frame and compares two digests against the local
    // ledger; it opens no socket at the device, writes nothing, and cannot
    // change what the panel shows. The word guards *reaching the hardware*,
    // and the send dialog's first act is a dry run — so requiring it here
    // meant the review screen, which exists so somebody can look at the frame
    // before deciding, could not be shown until they had already typed the
    // confirmation for sending it. The gate below on the real push is
    // unchanged and is the one that matters.
    if (!body.dryRun && !simulated && body.confirm !== REAL_PUSH_CONFIRMATION) {
      appendAudit({
        action: "device.push",
        target: `${dashboardId}@v${version}`,
        outcome: "refused",
        deviceConfirmed: false,
        detail: "Real push attempted without the typed confirmation",
      });
      return jsonError(
        400,
        "confirmation_required",
        `Type ${REAL_PUSH_CONFIRMATION} to confirm a push to the real device`,
      );
    }

    const now = new Date();
    // Forced: a push paints frozen ink, so the one place that is worth
    // bypassing the Reminders cache for is this one. See CollectOptions.
    const sources = await collectSources(doc, now, { force: true });

    if (body.dryRun) {
      const prepared = prepareFrame(doc, sources, now);
      const previous = lastVerifiedPush();
      return NextResponse.json({
        dryRun: true,
        sha256: prepared.sha256,
        semanticHash: prepared.semanticHash,
        wouldDedup: previous?.first.semanticHash === prepared.semanticHash,
        bytes: prepared.bytes.length,
      });
    }

    const result = await push({
      doc,
      version,
      sources,
      client,
      deviceMode: simulated ? "mock" : "real",
      force: body.force,
      now,
      // A person asked for this frame. A device that is asleep has not refused
      // it — it is not listening, and nothing on the network can change that —
      // so the frame is held and delivered at the next window rather than
      // thrown away with a red badge. The confirmation above has already been
      // typed by this point, so a queued push is a push the user confirmed.
      // See src/core/pushQueue.ts for what is NOT queued: anything the device
      // answered and refused.
      queueWhenUnreachable: true,
    });

    const status =
      result.outcome === "blocked"
        ? 409
        : result.outcome === "would_dedup"
          ? 409
          : result.outcome === "failed"
            ? 502
            : // 202: accepted here, not yet performed there. The one status
              // code that means exactly what a queued push is.
              result.outcome === "queued"
              ? 202
              : 200;

    // The device has changed, so the Overview snapshot is now describing a
    // panel that no longer exists. Dropped rather than refreshed: the page
    // reloads with `force=1` straight after a push anyway, and rebuilding it
    // here would be a second device read inside the same request.
    invalidateOverviewSnapshot();

    return NextResponse.json({ result }, { status });
  },
);
