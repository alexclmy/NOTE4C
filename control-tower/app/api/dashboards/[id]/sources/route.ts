import { NextResponse } from "next/server";
import { z } from "zod";
import { guarded, jsonError } from "@/server/auth/guard";
import { collectSources, sourceHealth } from "@/server/sources";
import { readRecord } from "@/server/store/dashboards";
import type { DashboardDoc } from "@/core/model";

export const dynamic = "force-dynamic";

function idFrom(pathname: string): string {
  const parts = pathname.split("/").filter(Boolean);
  return parts[parts.length - 2] ?? "";
}

/**
 * Live source data for the designer preview, so what the browser draws is what
 * the panel would actually get. Only the sources this dashboard binds to are
 * fetched; the tower never reads something a layout does not use.
 *
 * GET binds against the SAVED record — the right answer on first load. But the
 * designer edits a draft that is not saved on every keystroke, and a module you
 * just dropped is not in the saved record yet, so a weather block added to a
 * Blank composition would read "unavailable" until the first save. POST fixes
 * that: it binds against the working DRAFT the browser sends, so a module shows
 * its real data the instant it is placed, on any template — Blank included.
 *
 * The draft schema is deliberately permissive (it validates only what binding
 * needs: each module's type and options), so a mid-edit layout — one module
 * briefly overlapping another during a drag — still returns data rather than a
 * validation error. Reminders are still read only when a module asks for them;
 * nothing private is collected speculatively.
 */
const DraftModule = z
  .object({
    type: z.string(),
    options: z.record(z.string(), z.unknown()).optional(),
  })
  .passthrough();

const DraftBody = z.object({
  doc: z
    .object({ modules: z.array(DraftModule).max(64) })
    .passthrough(),
});

async function sourcesFor(doc: DashboardDoc, now: Date): Promise<NextResponse> {
  const sources = await collectSources(doc, now);
  return NextResponse.json({
    readAt: now.toISOString(),
    sources,
    health: sourceHealth(sources),
  });
}

export const GET = guarded({}, async ({ request }) => {
  const id = idFrom(request.nextUrl.pathname);
  const record = readRecord(id);
  if (!record) return jsonError(404, "not_found", `No dashboard with id "${id}"`);
  return sourcesFor(record.doc, new Date());
});

export const POST = guarded(
  { mutating: true, schema: DraftBody, action: "dashboard.sources.draft" },
  async ({ request, body }) => {
    const id = idFrom(request.nextUrl.pathname);
    // The id must still name a real dashboard — this is a preview of one, not a
    // way to collect sources for a document that was never created.
    if (!readRecord(id)) {
      return jsonError(404, "not_found", `No dashboard with id "${id}"`);
    }
    return sourcesFor(body.doc as unknown as DashboardDoc, new Date());
  },
);
