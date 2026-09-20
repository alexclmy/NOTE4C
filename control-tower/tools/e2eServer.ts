/**
 * The web server the Playwright suite runs against: build once, then serve.
 *
 * WHY NOT `next dev`
 * -----------------
 * The suite used to point Playwright's `webServer` at `next dev`, and the
 * development server compiles on demand. The first request for a route pays
 * for its compilation, and — the part that actually broke tests — the dev
 * bundler occasionally decides a module graph cannot be patched in place and
 * tells the open page to reload itself:
 *
 *     ⚠ Fast Refresh had to perform a full reload
 *
 * A full reload in the middle of a spec throws away everything the test just
 * typed into the designer, so an assertion two lines later reads a canvas that
 * was repainted from a freshly mounted component. That is how
 * tests/e2e/08-typography.spec.ts came to fail one test per full run, a
 * different one each time, only on the first (desktop) project, and never when
 * the file was run on its own: running the whole suite is what keeps reaching
 * new routes and new modules, and so keeps giving the bundler new reasons to
 * recompile while a test is mid-flight.
 *
 * A production build has none of that. Every route is compiled before the
 * first browser opens, nothing recompiles while the suite runs, and there is
 * no HMR socket to reload anything. The suite is slower to start by the length
 * of one build and stops being a coin flip.
 *
 * WHAT THIS SCRIPT IS FOR
 * -----------------------
 * `webServer.command` is a single command, and this is two: `next build`, then
 * `next start` on the same host, port and dist directory. Sequencing them in a
 * shell string (`next build && next start`) would work on this machine and
 * hide the interesting parts — which dist directory was built, whether the
 * build output is worth printing, what happens to the server on SIGTERM — so
 * they live here instead, with the same shape as tools/tower-serve.ts: stay in
 * the foreground, stay the parent, forward signals, exit with the child's
 * status.
 *
 * THE DIST DIRECTORY
 * ------------------
 * `next start` serves out of `next.config.mjs`'s `distDir`, which this repo
 * takes from NEXT_DIST_DIR. Both phases therefore have to run with the same
 * value in the environment, and this process simply inherits the one
 * playwright.config.ts sets. It is checked rather than trusted: building into
 * `.next` is precisely what tools/nextDistIsolation.ts exists to prevent,
 * because `.next` is the directory a running tower is serving the household's
 * dashboard out of.
 */

import { spawn } from "node:child_process";
import os from "node:os";
import { assertScratchDistDir } from "./nextDistIsolation";

interface Bind {
  hostname: string;
  port: string;
}

function usage(): never {
  console.error(
    "usage: tsx tools/e2eServer.ts --hostname <host> --port <port>",
  );
  process.exit(2);
}

function parseArgs(argv: readonly string[]): Bind {
  let hostname: string | undefined;
  let port: string | undefined;
  for (let i = 0; i < argv.length; i += 1) {
    const arg = argv[i] ?? "";
    if (arg === "--hostname" || arg === "-H") hostname = argv[(i += 1)];
    else if (arg === "--port" || arg === "-p") port = argv[(i += 1)];
    else usage();
  }
  if (hostname === undefined || port === undefined) usage();
  if (!/^\d+$/.test(port)) usage();
  return { hostname, port };
}

/** Everything this script says goes to stderr: Playwright pipes that one. */
function say(message: string): void {
  console.error(`[e2e-server] ${message}`);
}

function run(
  command: string,
  args: readonly string[],
  options: { capture: boolean },
): Promise<{ code: number; output: string }> {
  return new Promise((resolve, reject) => {
    const child = spawn(command, args, {
      stdio: options.capture ? ["ignore", "pipe", "pipe"] : "inherit",
      env: process.env,
    });

    let output = "";
    if (options.capture) {
      child.stdout?.on("data", (chunk: Buffer) => {
        output += chunk.toString();
      });
      child.stderr?.on("data", (chunk: Buffer) => {
        output += chunk.toString();
      });
    }

    for (const signal of ["SIGINT", "SIGTERM", "SIGHUP"] as const) {
      process.on(signal, () => {
        child.kill(signal);
      });
    }

    child.on("error", reject);
    child.on("exit", (code, signal) => {
      if (signal) {
        resolve({ code: 128 + (os.constants.signals[signal] ?? 0), output });
        return;
      }
      resolve({ code: code ?? 0, output });
    });
  });
}

async function main(): Promise<void> {
  const { hostname, port } = parseArgs(process.argv.slice(2));

  const distDir = process.env.NEXT_DIST_DIR;
  if (distDir === undefined || distDir === "") {
    say(
      "NEXT_DIST_DIR is not set. This server must build into a scratch tree, " +
        "never into .next, which is what a running tower serves from.",
    );
    process.exit(2);
  }
  // Throws with the explanation if it is `.next`, a path, or anything else
  // that is not a `.next-<name>` scratch segment.
  assertScratchDistDir(distDir);

  // The build is quiet when it succeeds and printed in full when it does not.
  // A green run should not bury the test report under Next's route table, and
  // a red one is unreadable without it.
  const started = Date.now();
  say(`building into ${distDir} (production build, no compile-on-demand)`);
  const build = await run("npx", ["next", "build"], { capture: true });
  if (build.code !== 0) {
    say(`next build failed with status ${build.code}:`);
    console.error(build.output);
    process.exit(build.code);
  }
  say(`built in ${((Date.now() - started) / 1000).toFixed(1)}s`);

  say(`next start on ${hostname}:${port}`);
  const serve = await run(
    "npx",
    ["next", "start", "--hostname", hostname, "--port", port],
    { capture: false },
  );
  process.exitCode = serve.code;
}

void main();
