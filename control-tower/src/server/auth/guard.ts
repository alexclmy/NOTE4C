import { NextResponse } from "next/server";
import type { NextRequest } from "next/server";
import { z } from "zod";
import { appendAudit } from "../audit";
import { passphraseIsSet } from "./passphrase";
import {
  CSRF_COOKIE,
  CSRF_HEADER,
  SESSION_COOKIE,
  csrfMatches,
  verifySession,
} from "./session";

/**
 * One gate for every API route.
 *
 * Reads require a session. Mutations require a session AND a matching
 * double-submit CSRF token. There is no route that skips both, and the two
 * public routes (first-run state, login) are explicit rather than implied.
 */

export type ApiHandler<T> = (context: {
  request: NextRequest;
  body: T;
}) => Promise<NextResponse> | NextResponse;

export function jsonError(
  status: number,
  code: string,
  detail: string,
  extra: Record<string, unknown> = {},
): NextResponse {
  return NextResponse.json({ error: code, detail, ...extra }, { status });
}

export function isAuthenticated(request: NextRequest): boolean {
  return verifySession(request.cookies.get(SESSION_COOKIE)?.value) !== null;
}

export interface GuardOptions<T> {
  /** Mutating routes get the CSRF check and an audit entry on refusal. */
  mutating?: boolean;
  schema?: z.ZodType<T, z.ZodTypeDef, unknown>;
  /** Named actions appear in the audit log when a request is refused. */
  action?: string;
}

/**
 * Wrap a route handler. Returns a Next route function.
 */
export function guarded<T = undefined>(
  options: GuardOptions<T>,
  handler: ApiHandler<T>,
): (request: NextRequest, context?: unknown) => Promise<NextResponse> {
  return async (request: NextRequest): Promise<NextResponse> => {
    if (!isAuthenticated(request)) {
      return jsonError(401, "unauthenticated", "Sign in to the tower first");
    }

    if (options.mutating) {
      const cookie = request.cookies.get(CSRF_COOKIE)?.value;
      const header = request.headers.get(CSRF_HEADER);
      if (!csrfMatches(cookie, header)) {
        appendAudit({
          action: options.action ?? "api.mutation",
          target: request.nextUrl.pathname,
          outcome: "refused",
          detail: "CSRF token missing or did not match",
        });
        return jsonError(403, "csrf_failed", "The CSRF token did not match");
      }
    }

    let body = undefined as T;
    if (options.schema) {
      let raw: unknown;
      try {
        raw = await request.json();
      } catch {
        return jsonError(400, "bad_body", "The request body is not valid JSON");
      }
      const parsed = options.schema.safeParse(raw);
      if (!parsed.success) {
        return jsonError(400, "bad_body", "The request body failed validation", {
          issues: parsed.error.issues.map((issue) => ({
            path: issue.path.join("."),
            message: issue.message,
          })),
        });
      }
      body = parsed.data;
    }

    try {
      return await handler({ request, body });
    } catch (error) {
      const detail =
        error instanceof Error ? error.message : "The request could not be completed";
      if (options.mutating) {
        appendAudit({
          action: options.action ?? "api.mutation",
          target: request.nextUrl.pathname,
          outcome: "failed",
          detail,
        });
      }
      // Validation and not-found errors carry useful, secret-free messages.
      const status = /not valid|does not match|already exists|archived|No dashboard|no version|not in the allowed set|must be/i.test(
        detail,
      )
        ? 400
        : 500;
      return jsonError(status, "request_failed", detail);
    }
  };
}

/** For the two routes that must work before a session exists. */
export function publicRoute(
  handler: (request: NextRequest) => Promise<NextResponse> | NextResponse,
): (request: NextRequest) => Promise<NextResponse> {
  return async (request: NextRequest): Promise<NextResponse> => {
    try {
      return await handler(request);
    } catch (error) {
      return jsonError(
        500,
        "request_failed",
        error instanceof Error ? error.message : "The request could not be completed",
      );
    }
  };
}

export function authState(request: NextRequest): {
  passphraseSet: boolean;
  authenticated: boolean;
} {
  return {
    passphraseSet: passphraseIsSet(),
    authenticated: isAuthenticated(request),
  };
}
