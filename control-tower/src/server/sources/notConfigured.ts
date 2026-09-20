/**
 * "This source has not been set up", said once.
 *
 * The renderer's contract has three states — ok, stale, unavailable — and that
 * is right for the panel: a tile whose data is missing draws its own explicit
 * unavailable mark, and *why* it is missing is not a distinction 400x300
 * pixels of e-paper should be spending ink on.
 *
 * The interface, though, needs the distinction badly. "Unavailable" about a
 * calendar source nobody configured reads as a fault and sends the operator
 * looking for a broken worker; "not configured" tells them there is a variable
 * to set. So the state stays `unavailable` on the wire the modules read, and
 * the detail carries a marker that sourceHealth turns into the fourth word the
 * UI already knows how to render.
 */

export const NOT_CONFIGURED_PREFIX = "Not configured.";

/** Build an unavailable detail that the UI will report as not configured. */
export function notConfigured(detail: string): string {
  return `${NOT_CONFIGURED_PREFIX} ${detail}`;
}

export function isNotConfigured(detail: string | undefined): boolean {
  return typeof detail === "string" && detail.startsWith(NOT_CONFIGURED_PREFIX);
}
