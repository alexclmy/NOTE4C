import { NextResponse } from "next/server";
import { z } from "zod";
import { appendAudit } from "@/server/audit";
import { guarded } from "@/server/auth/guard";
import { duplicateDashboard } from "@/server/store/dashboards";

export const dynamic = "force-dynamic";

function idFrom(pathname: string): string {
  const parts = pathname.split("/").filter(Boolean);
  return parts[parts.length - 2] ?? "";
}

const BodySchema = z.object({ title: z.string().min(1).max(80).optional() });

export const POST = guarded(
  { mutating: true, schema: BodySchema, action: "dashboard.duplicate" },
  async ({ request, body }) => {
    const id = idFrom(request.nextUrl.pathname);
    const record = duplicateDashboard(id, body.title);
    appendAudit({
      action: "dashboard.duplicate",
      target: record.doc.id,
      params: { source: id, title: record.doc.title },
      outcome: "ok",
      detail: "Copied with fresh module ids",
    });
    return NextResponse.json({ record }, { status: 201 });
  },
);
