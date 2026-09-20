import fs from "node:fs";
import path from "node:path";
import { z } from "zod";
import { DIR_MODE, FILE_MODE, ensureDir, paths } from "./paths";

/**
 * Durable atomic replace: write a temp file in the SAME directory, fsync it,
 * rename over the target, then fsync the directory so the rename itself is
 * durable. This is the pattern proven by note4c-dashboard's atomic() helper,
 * with the fsync steps the composer leaves to the OS.
 */
export function writeFileAtomic(
  filePath: string,
  data: Buffer | string,
  mode: number = FILE_MODE,
): void {
  const dir = path.dirname(filePath);
  ensureDir(dir);
  const buffer = typeof data === "string" ? Buffer.from(data, "utf8") : data;
  const tempPath = path.join(
    dir,
    `.${path.basename(filePath)}.tmp-${process.pid}-${Date.now().toString(36)}`,
  );
  let fd: number | null = null;
  try {
    fd = fs.openSync(tempPath, "wx", mode);
    fs.writeSync(fd, buffer);
    fs.fsyncSync(fd);
    fs.closeSync(fd);
    fd = null;
    fs.chmodSync(tempPath, mode);
    fs.renameSync(tempPath, filePath);
    fsyncDir(dir);
  } catch (error) {
    if (fd !== null) {
      try {
        fs.closeSync(fd);
      } catch {
        /* the original error is the one worth reporting */
      }
    }
    try {
      fs.unlinkSync(tempPath);
    } catch {
      /* temp file may not exist */
    }
    throw error;
  }
}

function fsyncDir(dir: string): void {
  let dirFd: number | null = null;
  try {
    dirFd = fs.openSync(dir, "r");
    fs.fsyncSync(dirFd);
  } catch {
    // Some filesystems refuse fsync on a directory handle. The rename is still
    // atomic; only the durability window widens, so this is not fatal.
  } finally {
    if (dirFd !== null) {
      try {
        fs.closeSync(dirFd);
      } catch {
        /* nothing further to do */
      }
    }
  }
}

export function writeJsonAtomic(
  filePath: string,
  value: unknown,
  mode: number = FILE_MODE,
): void {
  writeFileAtomic(filePath, `${JSON.stringify(value, null, 2)}\n`, mode);
}

/** Append one JSON line and fsync before returning. Used by the write-ahead ledger. */
export function appendJsonLine(
  filePath: string,
  value: unknown,
  mode: number = FILE_MODE,
): void {
  const dir = path.dirname(filePath);
  ensureDir(dir);
  const line = `${JSON.stringify(value)}\n`;
  const fd = fs.openSync(filePath, "a", mode);
  try {
    fs.writeSync(fd, line);
    fs.fsyncSync(fd);
  } finally {
    fs.closeSync(fd);
  }
  try {
    fs.chmodSync(filePath, mode);
  } catch {
    /* mode is already correct when we created the file */
  }
}

export function readJsonLines<T>(
  filePath: string,
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  schema: z.ZodType<T, z.ZodTypeDef, any>,
): T[] {
  let raw: string;
  try {
    raw = fs.readFileSync(filePath, "utf8");
  } catch {
    return [];
  }
  const out: T[] = [];
  for (const line of raw.split("\n")) {
    const trimmed = line.trim();
    if (trimmed.length === 0) continue;
    let parsed: unknown;
    try {
      parsed = JSON.parse(trimmed);
    } catch {
      // A torn final line can only happen if a write was interrupted mid-append.
      // Skip it rather than losing every earlier entry.
      continue;
    }
    const result = schema.safeParse(parsed);
    if (result.success) out.push(result.data);
  }
  return out;
}

export class DocumentCorruptError extends Error {
  constructor(filePath: string, detail: string) {
    super(`Document at ${filePath} failed validation: ${detail}`);
    this.name = "DocumentCorruptError";
  }
}

export interface DocumentSpec<T> {
  /**
   * Input side is loose because schemas use .default(), which makes their
   * input type optional while the parsed output stays complete.
   */
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  schema: z.ZodType<T, z.ZodTypeDef, any>;
  /** Current schema_version this build writes. */
  version: number;
  /** Forward-only migration from an older raw document. */
  migrate?: (raw: unknown, fromVersion: number) => unknown;
}

/**
 * Read a versioned document. Older documents are backed up before being
 * migrated forward and rewritten; history is never rewritten in place without
 * a copy landing in backups/.
 */
export function readDocument<T>(
  filePath: string,
  spec: DocumentSpec<T>,
): T | null {
  let raw: string;
  try {
    raw = fs.readFileSync(filePath, "utf8");
  } catch {
    return null;
  }
  let parsed: unknown;
  try {
    parsed = JSON.parse(raw);
  } catch (error) {
    throw new DocumentCorruptError(filePath, String(error));
  }

  const foundVersion =
    typeof parsed === "object" &&
    parsed !== null &&
    typeof (parsed as { schema_version?: unknown }).schema_version === "number"
      ? ((parsed as { schema_version: number }).schema_version as number)
      : 0;

  if (foundVersion > spec.version) {
    throw new DocumentCorruptError(
      filePath,
      `schema_version ${foundVersion} is newer than this build understands (${spec.version})`,
    );
  }

  if (foundVersion < spec.version) {
    backupFile(filePath, raw);
    if (!spec.migrate) {
      throw new DocumentCorruptError(
        filePath,
        `schema_version ${foundVersion} needs a migration and none is registered`,
      );
    }
    const migrated = spec.migrate(parsed, foundVersion);
    const validated = spec.schema.safeParse(migrated);
    if (!validated.success) {
      throw new DocumentCorruptError(filePath, validated.error.message);
    }
    writeJsonAtomic(filePath, validated.data);
    return validated.data;
  }

  const validated = spec.schema.safeParse(parsed);
  if (!validated.success) {
    throw new DocumentCorruptError(filePath, validated.error.message);
  }
  return validated.data;
}

export function writeDocument<T>(
  filePath: string,
  spec: DocumentSpec<T>,
  value: T,
): T {
  const validated = spec.schema.safeParse(value);
  if (!validated.success) {
    throw new DocumentCorruptError(filePath, validated.error.message);
  }
  writeJsonAtomic(filePath, validated.data);
  return validated.data;
}

export function backupFile(filePath: string, contents: string): string {
  const dir = ensureDir(paths.backupsDir());
  fs.chmodSync(dir, DIR_MODE);
  const stamp = new Date().toISOString().replace(/[:.]/g, "-");
  const target = path.join(dir, `${path.basename(filePath)}.${stamp}.bak`);
  writeFileAtomic(target, contents);
  return target;
}
