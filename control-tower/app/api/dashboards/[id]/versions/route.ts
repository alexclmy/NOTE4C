import { NextResponse } from "next/server";
import { z } from "zod";
import { DashboardDocSchema } from "@/core/model";
import { appendAudit } from "@/server/audit";
import { guarded } from "@/server/auth/guard";
import { listVersions, readVersion, saveVersion } from "@/server/store/dashboards";

export const dynamic = "force-dynamic";

function idFrom(pathname: string): string {
  const parts = pathname.split("/").filter(Boolean);
  // .../dashboards/<id>/versions
  return parts[parts.length - 2] ?? "";
}

export const GET = guarded({}, async ({ request }) => {
  const id = idFrom(request.nextUrl.pathname);
  const version = request.nextUrl.searchParams.get("version");
  if (version) {
    return NextResponse.json({ version: readVersion(id, Number(version)) });
  }
  return NextResponse.json({ versions: listVersions(id) });
});

const SaveSchema = z.object({
  doc: DashboardDocSchema,
  note: z.string().max(200).default(""),
});

export const POST = guarded(
  { mutating: true, schema: SaveSchema, action: "dashboard.saveVersion" },
  async ({ request, body }) => {
    const id = idFrom(request.nextUrl.pathname);
    const record = saveVersion(id, body.doc, body.note);
    appendAudit({
      action: "dashboard.saveVersion",
      target: `${id}@v${record.latestVersion}`,
      params: { note: body.note, moduleCount: body.doc.modules.length },
      outcome: "ok",
      detail: `Version ${record.latestVersion} saved`,
    });
    return NextResponse.json({ record }, { status: 201 });
  },
);
