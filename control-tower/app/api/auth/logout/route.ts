import { NextResponse } from "next/server";
import { appendAudit } from "@/server/audit";
import { clearSession } from "@/server/auth/cookies";
import { guarded } from "@/server/auth/guard";

export const dynamic = "force-dynamic";

export const POST = guarded({ mutating: true, action: "auth.logout" }, () => {
  appendAudit({
    action: "auth.logout",
    target: "tower",
    outcome: "ok",
    detail: "Signed out",
  });
  return clearSession(NextResponse.json({ ok: true }));
});
