import { NextResponse } from "next/server";
import type { NextRequest } from "next/server";
import { authState, publicRoute } from "@/server/auth/guard";

export const dynamic = "force-dynamic";

/**
 * Public by necessity: the login screen has to know whether this is a first
 * run or a sign-in. It reveals nothing beyond those two booleans.
 */
export const GET = publicRoute((request: NextRequest) =>
  NextResponse.json(authState(request)),
);
