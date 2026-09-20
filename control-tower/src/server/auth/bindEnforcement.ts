/**
 * Enforcing the bind policy inside a server that is already running.
 *
 * WHY THIS IS THE SECOND LINE AND NOT THE FIRST
 * ---------------------------------------------
 * A guard that runs after the socket is open has already lost: by the time
 * `instrumentation.ts` calls `register()`, Next has bound whatever it was told
 * to bind. The place to refuse a host is before the server exists, which is
 * what `tools/tower-serve.ts` does and why `npm run dev` and `npm start` go
 * through it.
 *
 * This file exists for the case that launcher cannot cover: somebody running
 * `npx next start --hostname 0.0.0.0` by hand, or a process manager with its
 * own command line. It observes what this process is actually listening on and
 * stops serving if that is a host the policy refuses.
 *
 * WHAT IT WILL AND WILL NOT CLAIM
 * -------------------------------
 * It reports the *source* of its observation, and "none" is a real answer. Not
 * every Next topology puts the listening socket in the process that runs
 * instrumentation — `next dev` compiles in a worker — so there are startups
 * where this cannot see the socket and says so rather than reporting a
 * loopback bind it did not verify. `GET /api/diagnostics` renders that word,
 * so the interface never claims a guard that did not run.
 *
 * Nothing here reconfigures a network, closes somebody else's socket, or
 * retries. The only action it takes is to refuse to go on.
 */

import {
  bindAllowed,
  hostFromArgv,
  isLoopbackHost,
  normaliseHost,
} from "./bindGuard";

/** Set by tools/tower-serve.ts to the host it already checked and approved. */
export const LAUNCHER_HOST_ENV = "NOTE4C_TOWER_BOUND_HOST";

export type BindSource =
  /** Read off a real listening socket held by this process. The strong case. */
  | "listening-socket"
  /** The launcher checked this host before Next started. */
  | "launcher"
  /** A `--hostname` on this process's own command line. */
  | "argv"
  /** Next's standalone server reads HOSTNAME from the environment. */
  | "env"
  /** Nothing in this process says what it bound. Not the same as loopback. */
  | "none";

export interface BindObservation {
  hosts: string[];
  source: BindSource;
}

/** The shape of the handles this cares about, without depending on node:net. */
interface AddressableHandle {
  address?: () => unknown;
}

function hostsFromHandles(handles: readonly unknown[]): string[] {
  const hosts: string[] = [];
  for (const handle of handles) {
    const address = (handle as AddressableHandle | null)?.address;
    if (typeof address !== "function") continue;
    let value: unknown;
    try {
      value = address.call(handle);
    } catch {
      // A handle that is closing answers by throwing. It is not listening.
      continue;
    }
    if (value === null || typeof value !== "object") continue;
    const record = value as { address?: unknown; port?: unknown };
    // A listening TCP server answers {address, family, port}. A connected
    // socket answers the same shape, which is why the port must be a number
    // AND the handle must not be a client: client sockets have a `remoteAddress`
    // and servers do not.
    if (typeof record.address !== "string" || typeof record.port !== "number") {
      continue;
    }
    if ("remoteAddress" in (handle as object)) continue;
    hosts.push(record.address);
  }
  return [...new Set(hosts)];
}

/**
 * What this process appears to be listening on, and how confidently.
 *
 * Injectable in full so the unit suite can drive every source, including the
 * ones that only occur under a process manager nobody wants to reproduce in a
 * test.
 */
export function observeBind(input: {
  handles?: readonly unknown[];
  argv?: readonly string[];
  env?: Record<string, string | undefined>;
}): BindObservation {
  const env = input.env ?? {};

  const socketHosts = hostsFromHandles(input.handles ?? []);
  if (socketHosts.length > 0) {
    return { hosts: socketHosts, source: "listening-socket" };
  }

  const launcher = env[LAUNCHER_HOST_ENV]?.trim();
  if (launcher !== undefined && launcher.length > 0) {
    return { hosts: [launcher], source: "launcher" };
  }

  const argvHost = hostFromArgv(input.argv ?? []);
  if (argvHost !== null) return { hosts: [argvHost], source: "argv" };

  const envHost = env.HOSTNAME?.trim();
  if (envHost !== undefined && envHost.length > 0) {
    return { hosts: [envHost], source: "env" };
  }

  return { hosts: [], source: "none" };
}

export type BindVerdict = "allowed" | "refused" | "undetermined";

export interface BindEnforcement {
  verdict: BindVerdict;
  source: BindSource;
  hosts: string[];
  /** One sentence per refused host. Empty when nothing was refused. */
  refusals: string[];
  /** One sentence, safe to print at startup and to show on Diagnostics. */
  summary: string;
}

/**
 * Apply the policy to an observation.
 *
 * Every observed host has to pass: a dual-stack listener reports two, and
 * "one of them is loopback" is not a reason to allow the other.
 */
export function enforceBind(
  observation: BindObservation,
  context: { passphraseSet: boolean; allowLanFlag: string | undefined },
): BindEnforcement {
  if (observation.hosts.length === 0) {
    return {
      verdict: "undetermined",
      source: observation.source,
      hosts: [],
      refusals: [],
      summary:
        "This process could not determine which address it is listening on, so the bind guard did not run here. The launcher (npm run dev / npm start) checks the host before the server starts; a server started another way is not covered.",
    };
  }

  const refusals: string[] = [];
  for (const host of observation.hosts) {
    const verdict = bindAllowed({
      hostname: host,
      passphraseSet: context.passphraseSet,
      allowLanFlag: context.allowLanFlag,
    });
    if (!verdict.allowed) {
      refusals.push(`${normaliseHost(host) || "(empty host)"}: ${verdict.reason}`);
    }
  }

  const list = observation.hosts.map((h) => normaliseHost(h) || "(empty host)").join(", ");
  if (refusals.length > 0) {
    return {
      verdict: "refused",
      source: observation.source,
      hosts: observation.hosts,
      refusals,
      summary: `The tower refused to serve on ${list}.`,
    };
  }

  const allLoopback = observation.hosts.every(isLoopbackHost);
  return {
    verdict: "allowed",
    source: observation.source,
    hosts: observation.hosts,
    refusals: [],
    summary: allLoopback
      ? `Listening on ${list}, which is loopback.`
      : `Listening on ${list}, allowed because a passphrase is set and NOTE4C_TOWER_ALLOW_LAN=1.`,
  };
}
