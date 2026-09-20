import { cookies } from "next/headers";
import { redirect } from "next/navigation";
import type { ReactNode } from "react";
import "@/ui/app.css";
import { AppShell } from "@/ui/AppShell";
import { SESSION_COOKIE, verifySession } from "@/server/auth/session";
import { readState } from "@/server/store/state";

export const dynamic = "force-dynamic";

/**
 * Auth guard for every application page. Checked on the server before any
 * markup is produced, so an unauthenticated visitor never sees a flash of the
 * shell before being redirected.
 */
export default async function AppLayout({ children }: { children: ReactNode }) {
  const jar = await cookies();
  if (!verifySession(jar.get(SESSION_COOKIE)?.value)) {
    redirect("/login");
  }

  const state = readState();
  const simulated = state.deviceMode === "mock";

  return (
    <AppShell
      simulated={simulated}
      deviceLabel={
        simulated
          ? "Mock device"
          : state.deviceAddress || "No device address set"
      }
      density={state.density}
    >
      {children}
    </AppShell>
  );
}
