/**
 * Give a throwaway QA server a build directory of its own.
 *
 * Two scripts in this repository start a Next server of their own: the
 * Playwright suite (playwright.config.ts, which now builds and then runs
 * `next start` — see tools/e2eServer.ts) and the screenshot run
 * (tools/screenshots.ts, still `next dev`). Left to itself, each builds into
 * `.next` — the very directory a production `next start` is serving chunks out
 * of. Running browser QA while the tower is up therefore rebuilds the running
 * server's assets underneath it, and the dashboard on the wall is being served
 * from a directory two other processes are writing to. `npm run build:check`
 * already refuses to do this for builds; the QA servers needed the same
 * treatment.
 *
 * Setting NEXT_DIST_DIR fixes that and creates one smaller problem. Both
 * commands run the same TypeScript setup step (`verifyTypeScriptSetup`: in
 * `next dev` via `setup-dev-bundler.js`, in `next build` as part of the build),
 * which rewrites `next-env.d.ts` and reformats `tsconfig.json` to point at
 * whatever dist directory it was handed. Both files are tracked. So the caller
 * keeps a byte copy of the two files before the server starts and puts them
 * back afterwards.
 *
 * Restoring *after* the run is enough, for the same reason in both flows: the
 * rewrite happens once, at the head of the run — the dev bundler verifies the
 * setup at startup and never again, and the build verifies it once before the
 * suite's first browser opens — and nothing touches the two files again while
 * tests are executing. `next start` never writes them at all.
 *
 * The copies are exact bytes, not `git checkout`. Uncommitted edits to
 * `tsconfig.json` are ordinary work, and a QA run is not entitled to discard
 * them.
 */

import fs from "node:fs";
import os from "node:os";
import path from "node:path";

/** The repository root; this file lives in tools/. */
const ROOT = path.resolve(__dirname, "..");

/** The two files Next rewrites to name the dist directory it was given. */
const GENERATED = ["next-env.d.ts", "tsconfig.json"] as const;

/**
 * The scratch build directories, named here rather than by their callers.
 *
 * Both the Playwright config and its global teardown need the e2e one, and
 * importing the config from the teardown would re-run the config's module body
 * at the wrong moment. One neutral home for the names avoids that entirely.
 */
export const E2E_DIST_DIR = ".next-e2e";
export const SCREENSHOT_DIST_DIR = ".next-screenshots";

/**
 * A dist directory name that is safe to hand to Next and to `rm -rf`.
 *
 * Next resolves distDir against the project root, so it must be a single
 * relative segment. The `.next-` prefix is required rather than merely
 * conventional: `.gitignore` ignores `/.next-*` directories, so a scratch tree
 * under that prefix can never show up as untracked noise or, worse, be
 * committed.
 */
export function assertScratchDistDir(distDir: string): void {
  if (!/^\.next-[A-Za-z0-9._-]+$/.test(distDir) || distDir.includes("..")) {
    throw new Error(
      `Refusing to use ${JSON.stringify(distDir)} as a scratch dist directory. ` +
        "It must be a single path segment matching .next-<name>, which keeps it " +
        "out of .next and inside the .gitignore rule for scratch builds.",
    );
  }
}

function keepDir(distDir: string): string {
  return path.join(os.tmpdir(), `note4c-tower-generated-${distDir}`);
}

/**
 * Copy the two generated files aside, before a QA build rewrites them.
 *
 * Self-healing rather than clobbering: if the files on disk already name the
 * scratch directory, a previous run was killed before it could restore them,
 * and the copies kept then are the pristine ones. Putting those back is right;
 * overwriting them with the mangled versions would make the damage permanent.
 */
export function preserveGeneratedFiles(distDir: string): void {
  assertScratchDistDir(distDir);
  const keep = keepDir(distDir);

  for (const name of GENERATED) {
    const live = path.join(ROOT, name);
    const kept = path.join(keep, name);
    if (!fs.existsSync(live)) continue;

    if (fs.readFileSync(live, "utf8").includes(distDir)) {
      if (fs.existsSync(kept)) fs.copyFileSync(kept, live);
      continue;
    }

    fs.mkdirSync(keep, { recursive: true });
    fs.copyFileSync(live, kept);
  }
}

/** Put the exact bytes back, and forget the copies. */
export function restoreGeneratedFiles(distDir: string): void {
  assertScratchDistDir(distDir);
  const keep = keepDir(distDir);

  for (const name of GENERATED) {
    const kept = path.join(keep, name);
    if (!fs.existsSync(kept)) continue;
    const live = path.join(ROOT, name);
    // Only write when it differs, so an untouched file keeps its mtime and no
    // watcher anywhere is woken for nothing.
    const wanted = fs.readFileSync(kept, "utf8");
    if (!fs.existsSync(live) || fs.readFileSync(live, "utf8") !== wanted) {
      fs.writeFileSync(live, wanted);
    }
    fs.rmSync(kept, { force: true });
  }
  fs.rmSync(keep, { recursive: true, force: true });
}
