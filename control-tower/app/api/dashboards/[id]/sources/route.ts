import { NextResponse } from "next/server";
import { guarded, jsonError } from "@/server/auth/guard";
import { collectSources, sourceHealth } from "@/server/sources";
import { readRecord } from "@/server/store/dashboards";

export const dynamic = "force-dynamic";

function idFrom(pathname: string): string {
  const parts = pathname.split("/").filter(Boolean);
  return parts[parts.length - 2] ?? "";
}

/**
 * Live source data for the designer preview, so what the browser draws is what
 * the panel would actually get. Only the sources this dashboard binds to are
 * fetched; the tower never reads something a layout does not use.
 */
export const GET = guarded({}, async ({ request }) => {
  const id = idFrom(request.nextUrl.pathname);
  const record = readRecord(id);
  if (!record) return jsonError(404, "not_found", `No dashboard with id "${id}"`);

  const now = new Date();
  const sources = await collectSources(record.doc, now);
  return NextResponse.json({
    readAt: now.toISOString(),
    sources,
    health: sourceHealth(sources),
  });
});
