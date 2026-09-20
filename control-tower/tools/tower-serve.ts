/**
 * The guarded way to start the tower.
 *
 *     npm run dev      →  tsx tools/tower-serve.ts dev
 *     npm start        →  tsx tools/tower-serve.ts start
 *
 * WHY THE LAUNCHER EXISTS
 * -----------------------
 * `src/server/auth/bindGuard.ts` has always held the policy — loopback unless
 * BOTH a passphrase and NOTE4C_TOWER_ALLOW_LAN=1, and never every interface —
 * and for a while nothing called it. The README described a guard, the unit
 * suite tested a pure function, and the actual bind was a `--hostname` string
 * in package.json that no code ever looked at.
 *
 * A guard that runs inside the server is too late by construction: by then the
 * socket is open. So the decision moves here, in front of Next. This process
 * resolves the host, applies the policy, and either execs Next or prints the
 * refusal and exits without ever opening a socket.
 *
 * COMPATIBILITY WITH launchd AND OTHER SUPERVISORS
 * ------------------------------------------------
 * It stays in the foreground and stays the parent: `spawn` with inherited
 * stdio, signals forwarded, and this process exits with the child's own status.
 * A LaunchAgent (or a systemd unit) that runs `npm start` therefore supervises
 * one process tree whose lifetime is the server's, which is what `KeepAlive`
 * needs. It never daemonises, never double-forks, and writes no pidfile.
 *
 * It also exports `NOTE4C_TOWER_BOUND_HOST` into the child, so the in-process
 * check in `instrumentation.ts` can tell "the launcher approved this host"
 * apart from "nobody knows what this bound".
 */

import { spawn } from "node:child_process";
import os from "node:os";
import { LAUNCHER_HOST_ENV } from "@/server/auth/bindEnforcement";
import {
  BindRefusedError,
  assertBindAllowed,
  isLoopbackHost,
  normaliseHost,
  resolveRequestedBind,
} from "@/server/auth/bindGuard";
import { passphraseIsSet } from "@/server/auth/passphrase";

const MODES = new Set(["dev", "start"]);

function usage(): never {
  console.error("usage: tsx tools/tower-serve.ts <dev|start> [next options]");
  process.exit(2);
}

function main(): void {
  const [mode, ...rest] = process.argv.slice(2);
  if (mode === undefined || !MODES.has(mode)) usage();

  const requested = resolveRequestedBind({ argv: rest, env: process.env });
  const host = normaliseHost(requested.host);
  const port = requested.port;

  // Read before the policy runs, not inside it: whether a passphrase exists is
  // a fact about the data root, and the policy is a pure function of facts.
  let passphraseSet = false;
  try {
    passphraseSet = passphraseIsSet();
  } catch (error) {
    console.error(
      `Could not read the tower data root, so the bind guard is treating this install as having no passphrase: ${
        error instanceof Error ? error.message : String(error)
      }`,
    );
  }

  try {
    assertBindAllowed({
      hostname: host,
      passphraseSet,
      allowLanFlag: process.env.NOTE4C_TOWER_ALLOW_LAN,
    });
  } catch (error) {
    if (!(error instanceof BindRefusedError)) throw error;
    console.error("");
    console.error(
      `NOTE4C control tower: refusing to bind ${host || "(empty host)"}.`,
    );
    console.error(`  ${error.message}`);
    console.error(
      `  The host came from ${
        requested.from === "argv"
          ? "a --hostname argument"
          : requested.from === "env"
            ? "NOTE4C_TOWER_HOST"
            : "the built-in default"
      }.`,
    );
    console.error("  No socket was opened.");
    console.error("");
    process.exit(1);
  }

  if (!isLoopbackHost(host)) {
    // Allowed, and still worth saying out loud every single time. The tower
    // holds a credential that can repaint a wall.
    console.error(
      `NOTE4C control tower: binding ${host}, which is not loopback. A passphrase is set and NOTE4C_TOWER_ALLOW_LAN=1, so this is allowed. Anything that can reach ${host}:${port} can reach the login page.`,
    );
  }

  const child = spawn(
    "npx",
    ["next", mode, "--hostname", host, "--port", port, ...stripBindArgs(rest)],
    {
      stdio: "inherit",
      env: { ...process.env, [LAUNCHER_HOST_ENV]: host },
    },
  );

  // Forward rather than ignore: a supervisor stopping this process means stop
  // the server, and a launcher that swallowed SIGTERM would leave Next running
  // with nothing watching it.
  for (const signal of ["SIGINT", "SIGTERM", "SIGHUP"] as const) {
    process.on(signal, () => {
      child.kill(signal);
    });
  }

  child.on("exit", (code, signal) => {
    if (signal) {
      // Report the signal the way a shell would, so `launchctl` sees the same
      // status it would have seen supervising Next directly.
      process.exitCode = 128 + (os.constants.signals[signal] ?? 0);
      return;
    }
    process.exitCode = code ?? 0;
  });
}

/**
 * Remove the host and port arguments we have already resolved.
 *
 * Passing them through as well would put two `--hostname` flags on Next's
 * command line, and the one that wins would be the operator's rather than the
 * one the guard approved.
 */
function stripBindArgs(argv: readonly string[]): string[] {
  const out: string[] = [];
  for (let i = 0; i < argv.length; i += 1) {
    const arg = argv[i] ?? "";
    if (arg === "--hostname" || arg === "-H" || arg === "--port" || arg === "-p") {
      const next = argv[i + 1];
      if (next !== undefined && !next.startsWith("-")) i += 1;
      continue;
    }
    if (/^(?:--hostname|-H|--port|-p)=/.test(arg)) continue;
    out.push(arg);
  }
  return out;
}

main();
