import { z } from "zod";
import { composerOrigin } from "@/server/config";
import { SourceError, fetchJson } from "./http";
import { isE2E } from "./e2eFixtures";

/**
 * A second, separate renderer. Read-only, and shown on Overview
 * as a separate system's health, clearly distinct from tower-rendered
 * dashboards. The tower never writes to it, never takes its scheduler lock,
 * and never calls its legacy gallery upload path.
 *
 * Its absence is a normal state, not an error: the composer is a plain
 * foreground process, not a service, and it is often simply not running.
 */
/**
 * The origin of that composer, from NOTE4C_COMPOSER_ORIGIN, or empty.
 *
 * Empty is the default and the honest one for a fresh installation: this is a
 * companion to one specific other program, and a tower that has never heard of
 * it should not report "not running" about it forever. When this is empty the
 * card disappears from Overview and Diagnostics entirely.
 */
export function composerStatusUrl(): string {
  const origin = composerOrigin();
  return origin.length > 0 ? `${origin.replace(/\/+$/, "")}/dashboard/status` : "";
}

const ComposerStatusSchema = z.object({
  checked_at: z.string().optional(),
  rendered_at: z.string().optional(),
  semantic_sha256: z.string().optional(),
  changed: z.boolean().optional(),
  auto_push: z.boolean().optional(),
  timezone: z.string().optional(),
  image_sha256: z.string().optional(),
  next_check_at: z.string().optional(),
  sources: z
    .record(
      z.string(),
      z.object({
        state: z.string(),
        checked_at: z.string().optional(),
        error: z.string().optional(),
      }),
    )
    .optional(),
});
export type ComposerStatus = z.infer<typeof ComposerStatusSchema>;

export type ComposerFeed =
  | { state: "running"; status: ComposerStatus; observedAt: string }
  | { state: "not_running"; detail: string }
  | { state: "unreadable"; detail: string }
  /** No origin configured: there is no such program here to report on. */
  | { state: "not_configured"; detail: string };

export async function readComposerFeed(
  now: Date = new Date(),
): Promise<ComposerFeed> {
  // Configuration first, and before the test short-circuit: an installation
  // that has not named an origin has no such program to report on, and that is
  // true in a browser run as much as anywhere else.
  const url = composerStatusUrl();
  if (url.length === 0) {
    return {
      state: "not_configured",
      detail:
        "No composer origin is configured. Set NOTE4C_COMPOSER_ORIGIN to read one.",
    };
  }

  // Browser QA does not knock on the composer's door. It is a real service on
  // this machine, read-only or not, and a suite about the user interface has
  // no business depending on whether somebody left it running.
  if (isE2E()) {
    return { state: "not_running", detail: "Composer is not running" };
  }

  try {
    const payload = await fetchJson(url, {
      timeoutMs: 2_000,
      // The composer checks the Host header against the origin it was asked
      // for. Requesting that origin already produces exactly this value;
      // sending it explicitly documents the requirement rather than relying on
      // that coincidence.
      headers: { host: new URL(url).host },
    });
    const parsed = ComposerStatusSchema.safeParse(payload);
    if (!parsed.success) {
      return { state: "unreadable", detail: "Composer status shape unexpected" };
    }
    return {
      state: "running",
      status: parsed.data,
      observedAt: now.toISOString(),
    };
  } catch (error) {
    const detail = error instanceof SourceError ? error.message : "Unreachable";
    // A refused connection or a timeout both mean "not running right now".
    if (/Timed out|TypeError|Request failed|fetch/i.test(detail)) {
      return { state: "not_running", detail: "Composer is not running" };
    }
    return { state: "unreadable", detail };
  }
}
