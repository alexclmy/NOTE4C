import fs from "node:fs";
import os from "node:os";
import path from "node:path";

/**
 * Private data root. Never inside the git repo: the repo holds source only.
 * Overridable for tests through NOTE4C_TOWER_DATA_DIR.
 */
export function dataRoot(): string {
  const override = process.env.NOTE4C_TOWER_DATA_DIR;
  if (override && override.length > 0) return path.resolve(override);
  return path.join(os.homedir(), ".note4c-control-tower");
}

export const DIR_MODE = 0o700;
export const FILE_MODE = 0o600;

function ensureDir(dir: string): string {
  fs.mkdirSync(dir, { recursive: true, mode: DIR_MODE });
  // mkdirSync honours umask, so force the mode we actually require.
  fs.chmodSync(dir, DIR_MODE);
  return dir;
}

export function ensureDataRoot(): string {
  const root = ensureDir(dataRoot());
  for (const sub of ["dashboards", "ledger", "queue", "secrets", "audit", "backups"]) {
    ensureDir(path.join(root, sub));
  }
  return root;
}

export const paths = {
  root: (): string => dataRoot(),
  state: (): string => path.join(dataRoot(), "state.json"),
  dashboards: (): string => path.join(dataRoot(), "dashboards"),
  dashboardDir: (id: string): string => path.join(dataRoot(), "dashboards", id),
  dashboardRecord: (id: string): string =>
    path.join(dataRoot(), "dashboards", id, "record.json"),
  dashboardVersionsDir: (id: string): string =>
    path.join(dataRoot(), "dashboards", id, "versions"),
  dashboardVersion: (id: string, n: number): string =>
    path.join(
      dataRoot(),
      "dashboards",
      id,
      "versions",
      `${String(n).padStart(4, "0")}.json`,
    ),
  ledger: (): string => path.join(dataRoot(), "ledger", "push-ledger.jsonl"),
  /**
   * Where the bytes of a push that could not be delivered wait.
   *
   * The frame itself, not a promise to re-render one: re-rendering at delivery
   * time would produce different pixels and a different digest from the ones
   * the ledger already recorded as this push's identity, which would make the
   * write-ahead record a lie and break the verification that compares the
   * device's displayed digest against it. 30000 bytes, at most one at a time,
   * removed the moment the push reaches any terminal state.
   */
  queueDir: (): string => path.join(dataRoot(), "queue"),
  queuedFrame: (pushId: string): string =>
    path.join(dataRoot(), "queue", `${pushId}.bin`),
  secretsDir: (): string => path.join(dataRoot(), "secrets"),
  secret: (name: string): string => path.join(dataRoot(), "secrets", name),
  auditDir: (): string => path.join(dataRoot(), "audit"),
  auditFile: (d: Date): string =>
    path.join(
      dataRoot(),
      "audit",
      `audit-${d.getUTCFullYear()}-${String(d.getUTCMonth() + 1).padStart(2, "0")}.jsonl`,
    ),
  backupsDir: (): string => path.join(dataRoot(), "backups"),
};

export { ensureDir };
