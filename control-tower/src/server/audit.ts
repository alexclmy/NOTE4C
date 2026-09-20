import { z } from "zod";
import { appendJsonLine, readJsonLines } from "./store/atomicFile";
import { ensureDataRoot, paths } from "./store/paths";

/**
 * Audit log for every mutating action.
 *
 * Records what was attempted, against what, with which parameters, what came
 * back, and crucially whether the DEVICE confirmed it. A tower that says
 * "done" without a device confirmation is exactly the failure this product
 * exists to avoid, so the distinction is a field, not a footnote.
 *
 * Secrets never appear here. Fields that carry one are redacted at the
 * boundary rather than trusted not to be logged.
 */

const REDACTED = "[redacted]";

/** Any parameter whose name matches is replaced before it is written. */
const SECRET_KEY_PATTERN = /token|passphrase|password|secret|credential/i;

export const AuditEntrySchema = z.object({
  schema_version: z.literal(1),
  at: z.string(),
  action: z.string(),
  target: z.string(),
  params: z.record(z.string(), z.unknown()),
  /**
   * "pending" was added for the hybrid power feature and means something the
   * other four cannot express: the tower recorded what the user asked for and
   * has not been able to deliver it yet, because the device is asleep. It is
   * not "failed" (nothing went wrong), not "refused" (nobody refused it) and
   * not "uncertain" (the tower knows exactly what happened). Widening the enum
   * is backward compatible: every entry already written uses one of the four.
   */
  outcome: z.enum(["ok", "refused", "failed", "uncertain", "pending"]),
  /** null when the action never involved the device. */
  deviceConfirmed: z.boolean().nullable(),
  detail: z.string().nullable(),
});
export type AuditEntry = z.infer<typeof AuditEntrySchema>;

export function redactParams(params: Record<string, unknown>): Record<string, unknown> {
  const out: Record<string, unknown> = {};
  for (const [key, value] of Object.entries(params)) {
    if (SECRET_KEY_PATTERN.test(key)) {
      // Record only that a value was supplied, never its length or shape.
      out[key] = value === undefined || value === null || value === "" ? null : REDACTED;
      continue;
    }
    if (value && typeof value === "object" && !Array.isArray(value)) {
      out[key] = redactParams(value as Record<string, unknown>);
      continue;
    }
    out[key] = value;
  }
  return out;
}

export interface AuditInput {
  action: string;
  target: string;
  params?: Record<string, unknown>;
  outcome: AuditEntry["outcome"];
  deviceConfirmed?: boolean | null;
  detail?: string | null;
}

export function appendAudit(input: AuditInput): AuditEntry {
  ensureDataRoot();
  const now = new Date();
  const entry: AuditEntry = AuditEntrySchema.parse({
    schema_version: 1,
    at: now.toISOString(),
    action: input.action,
    target: input.target,
    params: redactParams(input.params ?? {}),
    outcome: input.outcome,
    deviceConfirmed: input.deviceConfirmed ?? null,
    detail: input.detail ?? null,
  });
  appendJsonLine(paths.auditFile(now), entry);
  return entry;
}

export function readAudit(months = 2): AuditEntry[] {
  ensureDataRoot();
  const entries: AuditEntry[] = [];
  const now = new Date();
  for (let back = 0; back < months; back += 1) {
    const when = new Date(
      Date.UTC(now.getUTCFullYear(), now.getUTCMonth() - back, 1),
    );
    entries.push(...readJsonLines(paths.auditFile(when), AuditEntrySchema));
  }
  return entries.sort((a, b) => b.at.localeCompare(a.at));
}
