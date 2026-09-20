import { NextResponse } from "next/server";
import {
  CSRF_COOKIE,
  SESSION_COOKIE,
  SESSION_TTL_SECONDS,
  issueSession,
} from "./session";

/**
 * Attach a fresh session to a response.
 *
 * The session cookie is HttpOnly so no script can read it. The CSRF cookie
 * deliberately is not: the page has to read it to echo it back in a header,
 * which is exactly what an attacker's page cannot do.
 */
export function attachSession(
  response: NextResponse,
  secure: boolean,
): NextResponse {
  const { token, csrf } = issueSession();

  response.cookies.set(SESSION_COOKIE, token, {
    httpOnly: true,
    sameSite: "strict",
    secure,
    path: "/",
    maxAge: SESSION_TTL_SECONDS,
  });

  response.cookies.set(CSRF_COOKIE, csrf, {
    httpOnly: false,
    sameSite: "strict",
    secure,
    path: "/",
    maxAge: SESSION_TTL_SECONDS,
  });

  return response;
}

export function clearSession(response: NextResponse): NextResponse {
  for (const name of [SESSION_COOKIE, CSRF_COOKIE]) {
    response.cookies.set(name, "", { path: "/", maxAge: 0 });
  }
  return response;
}

export function requestIsSecure(url: string): boolean {
  try {
    return new URL(url).protocol === "https:";
  } catch {
    return false;
  }
}

/** A stable per-client key for the login rate limiter. */
export function clientKey(headers: Headers): string {
  return (
    headers.get("x-forwarded-for")?.split(",")[0]?.trim() ??
    headers.get("x-real-ip") ??
    "loopback"
  );
}
