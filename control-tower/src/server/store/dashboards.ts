import fs from "node:fs";
import path from "node:path";
import { z } from "zod";
import {
  DashboardDocSchema,
  assertValidDashboard,
  emptyDashboard,
  newDashboardId,
  newModuleId,
  normalizeDashboard,
  starterDashboard,
  type DashboardDoc,
} from "@/core/model";
import { migrateDashboardDoc } from "@/core/migrate";
import { readDocument, writeDocument, type DocumentSpec } from "./atomicFile";
import { ensureDataRoot, ensureDir, paths } from "./paths";
import { readState, updateState } from "./state";

export const VersionMetaSchema = z.object({
  version: z.number().int().positive(),
  savedAt: z.string(),
  note: z.string().max(200),
  /** Set when this version was created by rolling an older one forward. */
  rolledBackFrom: z.number().int().positive().nullable(),
});
export type VersionMeta = z.infer<typeof VersionMetaSchema>;

export const STORE_SCHEMA_VERSION = 5;

export const DashboardRecordSchema = z.object({
  schema_version: z.literal(STORE_SCHEMA_VERSION),
  doc: DashboardDocSchema,
  latestVersion: z.number().int().nonnegative(),
  versions: z.array(VersionMetaSchema),
});
export type DashboardRecord = z.infer<typeof DashboardRecordSchema>;

export const VersionDocSchema = z.object({
  schema_version: z.literal(STORE_SCHEMA_VERSION),
  version: z.number().int().positive(),
  savedAt: z.string(),
  note: z.string().max(200),
  rolledBackFrom: z.number().int().positive().nullable(),
  doc: DashboardDocSchema,
});
export type VersionDoc = z.infer<typeof VersionDocSchema>;

/**
 * Carry a stored file forward to the current schema.
 *
 * The file's own envelope and the dashboard document inside it move together,
 * so one migration covers both: bump the envelope, and hand the document to
 * the model's migration, which chains its own steps and dispatches per module.
 *
 * readDocument() copies the original into backups/ before this runs and
 * rewrites the file in place afterwards. For a VERSION file that rewrite is
 * still append-only in the sense that matters: the version number, its save
 * time, its note and its rolledBackFrom are all preserved, and no version is
 * added, removed or renumbered. What changes is the option shape inside the
 * snapshot, which has to change for this build to be able to read it at all.
 */
function migrateStoredDoc(raw: unknown, fromVersion: number): unknown {
  if (typeof raw !== "object" || raw === null) return raw;
  if (fromVersion >= STORE_SCHEMA_VERSION) return raw;
  const stored = raw as { doc?: unknown };
  return {
    ...stored,
    schema_version: STORE_SCHEMA_VERSION,
    doc: migrateDashboardDoc(stored.doc),
  };
}

const RECORD_SPEC: DocumentSpec<DashboardRecord> = {
  schema: DashboardRecordSchema,
  version: STORE_SCHEMA_VERSION,
  migrate: migrateStoredDoc,
};

const VERSION_SPEC: DocumentSpec<VersionDoc> = {
  schema: VersionDocSchema,
  version: STORE_SCHEMA_VERSION,
  migrate: migrateStoredDoc,
};

export class DashboardNotFoundError extends Error {
  constructor(id: string) {
    super(`No dashboard with id "${id}"`);
    this.name = "DashboardNotFoundError";
  }
}

export class VersionNotFoundError extends Error {
  constructor(id: string, version: number) {
    super(`Dashboard "${id}" has no version ${version}`);
    this.name = "VersionNotFoundError";
  }
}

/** Ids come from newDashboardId(); anything else must never reach the fs. */
function assertSafeId(id: string): string {
  if (!/^[A-Za-z0-9_-]{1,64}$/.test(id)) {
    throw new DashboardNotFoundError(id);
  }
  return id;
}

export function listDashboardIds(): string[] {
  ensureDataRoot();
  try {
    return fs
      .readdirSync(paths.dashboards(), { withFileTypes: true })
      .filter((entry) => entry.isDirectory())
      .map((entry) => entry.name)
      .filter((name) => /^[A-Za-z0-9_-]{1,64}$/.test(name))
      .sort();
  } catch {
    return [];
  }
}

export function readRecord(id: string): DashboardRecord | null {
  assertSafeId(id);
  ensureDataRoot();
  return readDocument(paths.dashboardRecord(id), RECORD_SPEC);
}

export function requireRecord(id: string): DashboardRecord {
  const record = readRecord(id);
  if (!record) throw new DashboardNotFoundError(id);
  return record;
}

function writeRecord(record: DashboardRecord): DashboardRecord {
  assertSafeId(record.doc.id);
  ensureDir(paths.dashboardDir(record.doc.id));
  ensureDir(paths.dashboardVersionsDir(record.doc.id));
  return writeDocument(paths.dashboardRecord(record.doc.id), RECORD_SPEC, record);
}

export function listDashboards(
  options: { includeArchived?: boolean } = {},
): DashboardRecord[] {
  const records: DashboardRecord[] = [];
  for (const id of listDashboardIds()) {
    let record: DashboardRecord | null = null;
    try {
      record = readRecord(id);
    } catch {
      // A corrupt record must not hide every other dashboard. It surfaces in
      // Diagnostics instead of taking the library down.
      continue;
    }
    if (!record) continue;
    if (!options.includeArchived && record.doc.status === "archived") continue;
    records.push(record);
  }
  return records.sort((a, b) =>
    a.doc.updatedAt === b.doc.updatedAt
      ? a.doc.title.localeCompare(b.doc.title)
      : b.doc.updatedAt.localeCompare(a.doc.updatedAt),
  );
}

export function createDashboard(
  title: string,
  options: { starter?: boolean; now?: Date } = {},
): DashboardRecord {
  const now = options.now ?? new Date();
  const doc =
    options.starter === false
      ? emptyDashboard(title, now)
      : starterDashboard(title, now);
  assertValidDashboard(doc);

  const record: DashboardRecord = {
    schema_version: STORE_SCHEMA_VERSION,
    doc: normalizeDashboard(doc),
    latestVersion: 0,
    versions: [],
  };
  writeRecord(record);

  // Every dashboard starts with version 1, so "roll back to how it began" is
  // always available and the version rail is never empty.
  return saveVersion(doc.id, record.doc, "Created", { now });
}

/**
 * Replace the working document. This is the editor's current state; it does
 * not create a version. Versions come from an explicit Save.
 */
export function updateDashboard(
  id: string,
  doc: DashboardDoc,
  options: { now?: Date } = {},
): DashboardRecord {
  const record = requireRecord(id);
  if (doc.id !== id) {
    throw new Error("Dashboard id in the body does not match the id in the path");
  }
  assertValidDashboard(doc);
  const next: DashboardRecord = {
    ...record,
    doc: normalizeDashboard({
      ...doc,
      createdAt: record.doc.createdAt,
      updatedAt: (options.now ?? new Date()).toISOString(),
    }),
  };
  return writeRecord(next);
}

export function saveVersion(
  id: string,
  doc: DashboardDoc,
  note = "",
  options: { now?: Date; rolledBackFrom?: number } = {},
): DashboardRecord {
  const record = requireRecord(id);
  if (doc.id !== id) {
    throw new Error("Dashboard id in the body does not match the id in the path");
  }
  assertValidDashboard(doc);

  const now = options.now ?? new Date();
  const version = record.latestVersion + 1;
  const savedAt = now.toISOString();
  const normalized = normalizeDashboard({
    ...doc,
    createdAt: record.doc.createdAt,
    updatedAt: savedAt,
  });

  const versionDoc: VersionDoc = {
    schema_version: STORE_SCHEMA_VERSION,
    version,
    savedAt,
    note: note.slice(0, 200),
    rolledBackFrom: options.rolledBackFrom ?? null,
    doc: normalized,
  };

  ensureDir(paths.dashboardVersionsDir(id));
  const target = paths.dashboardVersion(id, version);
  if (fs.existsSync(target)) {
    // Versions are append-only. Refusing here beats silently overwriting a
    // snapshot the user may have already pushed to the panel.
    throw new Error(`Version ${version} of "${id}" already exists`);
  }
  writeDocument(target, VERSION_SPEC, versionDoc);

  const meta: VersionMeta = {
    version,
    savedAt,
    note: versionDoc.note,
    rolledBackFrom: versionDoc.rolledBackFrom,
  };

  return writeRecord({
    ...record,
    doc: normalized,
    latestVersion: version,
    versions: [...record.versions, meta],
  });
}

export function listVersions(id: string): VersionMeta[] {
  return [...requireRecord(id).versions].sort((a, b) => b.version - a.version);
}

export function readVersion(id: string, version: number): VersionDoc {
  assertSafeId(id);
  if (!Number.isInteger(version) || version < 1) {
    throw new VersionNotFoundError(id, version);
  }
  const found = readDocument(paths.dashboardVersion(id, version), VERSION_SPEC);
  if (!found) throw new VersionNotFoundError(id, version);
  return found;
}

/**
 * Roll back by copying an old version forward as a new one. History is never
 * rewritten or truncated: the rolled-back-from version stays exactly where it
 * was, so the ledger's sha-to-version mapping stays valid forever.
 */
export function rollbackTo(
  id: string,
  version: number,
  options: { now?: Date } = {},
): DashboardRecord {
  const source = readVersion(id, version);
  return saveVersion(
    id,
    { ...source.doc, updatedAt: (options.now ?? new Date()).toISOString() },
    `Rolled back to version ${version}`,
    { ...options, rolledBackFrom: version },
  );
}

export function duplicateDashboard(
  id: string,
  title?: string,
  options: { now?: Date } = {},
): DashboardRecord {
  const source = requireRecord(id);
  const now = options.now ?? new Date();
  const stamp = now.toISOString();
  const copy: DashboardDoc = {
    ...source.doc,
    id: newDashboardId(),
    title: (title ?? `${source.doc.title} copy`).slice(0, 80),
    status: "active",
    // Fresh module ids so the two dashboards can never be confused in the
    // designer or in an audit entry.
    modules: source.doc.modules.map((module) => ({
      ...module,
      id: newModuleId(),
    })),
    createdAt: stamp,
    updatedAt: stamp,
  };
  assertValidDashboard(copy);

  writeRecord({
    schema_version: STORE_SCHEMA_VERSION,
    doc: normalizeDashboard(copy),
    latestVersion: 0,
    versions: [],
  });
  return saveVersion(copy.id, copy, `Duplicated from "${source.doc.title}"`, {
    now,
  });
}

export function setStatus(
  id: string,
  status: "active" | "archived",
  options: { now?: Date } = {},
): DashboardRecord {
  const record = requireRecord(id);
  const next = writeRecord({
    ...record,
    doc: {
      ...record.doc,
      status,
      updatedAt: (options.now ?? new Date()).toISOString(),
    },
  });

  // An archived dashboard must not stay selected for the next push.
  if (status === "archived") {
    const state = readState();
    if (state.selectedDashboardId === id) {
      updateState({ selectedDashboardId: null });
    }
  }
  return next;
}

export function archiveDashboard(id: string, now?: Date): DashboardRecord {
  return setStatus(id, "archived", now ? { now } : {});
}

export function restoreDashboard(id: string, now?: Date): DashboardRecord {
  return setStatus(id, "active", now ? { now } : {});
}

export function selectDashboard(id: string): void {
  const record = requireRecord(id);
  if (record.doc.status === "archived") {
    throw new Error("An archived dashboard cannot be selected for a push");
  }
  updateState({ selectedDashboardId: id });
}

/** Where a version file lives, for diagnostics. Never exposed over the API. */
export function versionPath(id: string, version: number): string {
  return path.join(paths.dashboardVersionsDir(assertSafeId(id)), `${String(version).padStart(4, "0")}.json`);
}
