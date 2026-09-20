import { NextResponse } from "next/server";
import { renderDashboard, PANEL_TIMEZONE } from "@/core/render";
import { guarded, jsonError } from "@/server/auth/guard";
import { framePng } from "@/server/png";
import { collectSources } from "@/server/sources";
import { emptySources } from "@/core/render/data";
import { readRecord, readVersion } from "@/server/store/dashboards";

export const dynamic = "force-dynamic";

function idFrom(pathname: string): string {
  const parts = pathname.split("/").filter(Boolean);
  return parts[parts.length - 2] ?? "";
}

/**
 * Server-rendered preview PNG, produced by the same renderer that packs the
 * bytes for the device. What this returns is what would ship.
 *
 * ?live=1 fetches real source data. Without it the preview shows every module
 * in its unavailable state, which is the honest default for a thumbnail that
 * must not imply data the tower has not actually read.
 */
export const GET = guarded({}, async ({ request }) => {
  const id = idFrom(request.nextUrl.pathname);
  const params = request.nextUrl.searchParams;

  const record = readRecord(id);
  if (!record) return jsonError(404, "not_found", `No dashboard with id "${id}"`);

  const versionParam = params.get("version");
  const doc = versionParam
    ? readVersion(id, Number(versionParam)).doc
    : record.doc;

  const now = new Date();
  const sources =
    params.get("live") === "1" ? await collectSources(doc, now) : emptySources();

  const png = framePng(
    renderDashboard(doc, sources, { now, timeZone: PANEL_TIMEZONE }),
  );

  return new NextResponse(new Uint8Array(png), {
    status: 200,
    headers: {
      "content-type": "image/png",
      "content-length": String(png.length),
      // A preview reflects live data and the current clock; never cache it.
      "cache-control": "no-store",
    },
  });
});
