import fs from "node:fs";
import os from "node:os";
import path from "node:path";

/**
 * Point the store at a throwaway data root for the duration of one test file.
 * Returns a disposer that restores the previous value and removes the tree.
 */
export function useTempDataRoot(): { root: string; dispose: () => void } {
  const previous = process.env.NOTE4C_TOWER_DATA_DIR;
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "note4c-tower-test-"));
  process.env.NOTE4C_TOWER_DATA_DIR = root;
  return {
    root,
    dispose: () => {
      if (previous === undefined) delete process.env.NOTE4C_TOWER_DATA_DIR;
      else process.env.NOTE4C_TOWER_DATA_DIR = previous;
      fs.rmSync(root, { recursive: true, force: true });
    },
  };
}

export function modeOf(target: string): number {
  return fs.statSync(target).mode & 0o777;
}
