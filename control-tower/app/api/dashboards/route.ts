import { NextResponse } from "next/server";
import { z } from "zod";
import { appendAudit } from "@/server/audit";
import { guarded } from "@/server/auth/guard";
import { createDashboard, listDashboards } from "@/server/store/dashboards";
import { readState } from "@/server/store/state";

export const dynamic = "force-dynamic";

export const GET = guarded({}, async ({ request }) => {
  const includeArchived =
    request.nextUrl.searchParams.get("includeArchived") === "1";
  const state = readState();
  return NextResponse.json({
    selectedDashboardId: state.selectedDashboardId,
    dashboards: listDashboards({ includeArchived }).map((record) => ({
      id: record.doc.id,
      title: record.doc.title,
      status: record.doc.status,
      moduleCount: record.doc.modules.length,
      latestVersion: record.latestVersion,
      updatedAt: record.doc.updatedAt,
      refreshIntervalMinutes: record.doc.refreshIntervalMinutes,
      createdAt: record.doc.createdAt,
    })),
  });
});

const CreateSchema = z.object({
  title: z.string().min(1).max(80),
  starter: z.boolean().default(true),
});

export const POST = guarded(
  { mutating: true, schema: CreateSchema, action: "dashboard.create" },
  async ({ body }) => {
    const record = createDashboard(body.title, { starter: body.starter });
    appendAudit({
      action: "dashboard.create",
      target: record.doc.id,
      params: { title: body.title, starter: body.starter },
      outcome: "ok",
      detail: `Created with ${record.doc.modules.length} modules`,
    });
    return NextResponse.json({ dashboard: record.doc, record }, { status: 201 });
  },
);
