import { NextResponse } from "next/server";
import { z } from "zod";
import { appendAudit } from "@/server/audit";
import { guarded } from "@/server/auth/guard";
import { rollbackTo } from "@/server/store/dashboards";

export const dynamic = "force-dynamic";

function idFrom(pathname: string): string {
  const parts = pathname.split("/").filter(Boolean);
  return parts[parts.length - 2] ?? "";
}

const BodySchema = z.object({ version: z.number().int().positive() });

export const POST = guarded(
  { mutating: true, schema: BodySchema, action: "dashboard.rollback" },
  async ({ request, body }) => {
    const id = idFrom(request.nextUrl.pathname);
    const record = rollbackTo(id, body.version);
    appendAudit({
      action: "dashboard.rollback",
      target: `${id}@v${record.latestVersion}`,
      params: { rolledBackFrom: body.version },
      outcome: "ok",
      detail: `Version ${body.version} copied forward as version ${record.latestVersion}; history untouched`,
    });
    return NextResponse.json({ record });
  },
);
