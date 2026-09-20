import fs from "node:fs";
import os from "node:os";
import path from "node:path";

/** Start every Playwright invocation with an isolated throwaway tower store. */
export default function globalSetup(): void {
  const dataRoot = path.join(os.tmpdir(), "note4c-tower-e2e");
  fs.rmSync(dataRoot, { recursive: true, force: true });
}
