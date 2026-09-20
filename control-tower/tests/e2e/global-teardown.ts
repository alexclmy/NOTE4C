import { E2E_DIST_DIR, restoreGeneratedFiles } from "../../tools/nextDistIsolation";

/**
 * Put `next-env.d.ts` and `tsconfig.json` back the way the run found them.
 *
 * The suite's web server builds into its own dist directory (see
 * tools/nextDistIsolation.ts), and the `next build` at the head of the run
 * rewrites those two tracked files to name whichever directory it was given.
 * Restoring them here means a browser QA run leaves the working tree exactly as
 * it found it, and `npm run typecheck` afterwards does not fail on a reference
 * to a scratch tree.
 *
 * The scratch tree itself is deliberately left on disk: it holds the
 * incremental build cache that makes the next run's build a fraction of a cold
 * one, and the `/.next-*` directory rule in .gitignore already keeps it
 * invisible to git.
 */
export default function globalTeardown(): void {
  restoreGeneratedFiles(E2E_DIST_DIR);
}
