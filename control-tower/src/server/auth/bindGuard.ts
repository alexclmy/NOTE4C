/**
 * Bind guard.
 *
 * The tower binds 127.0.0.1 and refuses anything else unless BOTH a passphrase
 * is configured AND the operator set NOTE4C_TOWER_ALLOW_LAN=1. Exposure is a
 * separately approved phase; nothing here ever touches Tailscale or any other
 * network configuration.
 *
 * This file is the *policy*: pure functions of their arguments, no process, no
 * sockets, no filesystem. Two callers enforce it, and they are deliberately
 * different in kind:
 *
 *  - `tools/tower-serve.ts`, the launcher `npm run dev` and `npm start` go
 *    through. It decides the host BEFORE Next exists, so a refused host means
 *    no socket is ever opened. That is the real protection.
 *  - `src/server/auth/bindEnforcement.ts`, called from `instrumentation.ts`
 *    inside the running server. It is defence in depth for the case where
 *    somebody ran `next start` directly, and it is honest about the fact that
 *    it can only refuse a bind it can actually observe.
 */

export class BindRefusedError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "BindRefusedError";
  }
}

export const LOOPBACK_HOSTS = new Set(["127.0.0.1", "::1", "localhost"]);

/** Every spelling of "listen on every interface" this runtime will accept. */
export const WILDCARD_HOSTS = new Set(["0.0.0.0", "::", "[::]", "*", ""]);

export interface BindContext {
  hostname: string;
  passphraseSet: boolean;
  allowLanFlag: string | undefined;
}

/**
 * Strip the brackets an IPv6 literal carries in a URL or a CLI argument.
 *
 * `--hostname [::]` and `--hostname ::` are the same request, and a guard that
 * recognised only one of them would refuse the tidy spelling and allow the
 * other. Also lowercases, because `::FFFF:0:0` and `::ffff:0:0` are one
 * address and a Set lookup does not know that.
 */
export function normaliseHost(hostname: string): string {
  const trimmed = hostname.trim().toLowerCase();
  if (trimmed.startsWith("[") && trimmed.endsWith("]")) {
    return trimmed.slice(1, -1);
  }
  return trimmed;
}

export function isLoopbackHost(hostname: string): boolean {
  const host = normaliseHost(hostname);
  if (LOOPBACK_HOSTS.has(host)) return true;
  if (/^127\.\d{1,3}\.\d{1,3}\.\d{1,3}$/.test(host)) return true;
  // The long spelling of ::1, and the IPv4-mapped loopback a dual-stack
  // listener reports. Both are the same socket as ::1 and 127.0.0.1.
  if (host === "0:0:0:0:0:0:0:1") return true;
  if (/^::ffff:127\.\d{1,3}\.\d{1,3}\.\d{1,3}$/.test(host)) return true;
  return false;
}

/** Does this host mean "every interface, including ones nobody meant"? */
export function isWildcardHost(hostname: string): boolean {
  const host = normaliseHost(hostname);
  if (WILDCARD_HOSTS.has(host)) return true;
  return host === "0:0:0:0:0:0:0:0" || host === "::ffff:0.0.0.0";
}

export function bindAllowed(context: BindContext): {
  allowed: boolean;
  reason: string;
} {
  if (isLoopbackHost(context.hostname)) {
    return { allowed: true, reason: "Loopback binding is always allowed" };
  }

  if (context.allowLanFlag !== "1") {
    return {
      allowed: false,
      reason:
        "Binding outside loopback needs NOTE4C_TOWER_ALLOW_LAN=1. Exposure is a separately approved step.",
    };
  }

  if (!context.passphraseSet) {
    return {
      allowed: false,
      reason:
        "Binding outside loopback needs a tower passphrase. Authentication is a precondition of any non-loopback exposure.",
    };
  }

  // 0.0.0.0 binds every interface, including any the operator did not mean.
  // Checked last on purpose: the two sentences above name a thing the operator
  // can go and fix, and a wildcard with neither of them set should hear about
  // the missing passphrase first.
  if (isWildcardHost(context.hostname)) {
    return {
      allowed: false,
      reason:
        "Binding every interface is refused. Name the one address the tower should listen on.",
    };
  }

  return { allowed: true, reason: "Passphrase set and LAN binding explicitly allowed" };
}

export function assertBindAllowed(context: BindContext): void {
  const verdict = bindAllowed(context);
  if (!verdict.allowed) throw new BindRefusedError(verdict.reason);
}

// ------------------------------------------------------------- CLI parsing --

/**
 * The host a `next dev` / `next start` argv is asking for, or null.
 *
 * Both spellings Next accepts, both forms of each: `--hostname h`,
 * `--hostname=h`, `-H h`, `-H=h`. Returns the LAST one, because that is which
 * one Next itself would use.
 */
export function hostFromArgv(argv: readonly string[]): string | null {
  let found: string | null = null;
  for (let i = 0; i < argv.length; i += 1) {
    const arg = argv[i] ?? "";
    if (arg === "--hostname" || arg === "-H") {
      const next = argv[i + 1];
      if (next !== undefined && !next.startsWith("-")) found = next;
      continue;
    }
    const match = /^(?:--hostname|-H)=(.*)$/.exec(arg);
    if (match) found = match[1] ?? "";
  }
  return found;
}

/** Same, for `--port p`, `--port=p`, `-p p`. Returns null when absent. */
export function portFromArgv(argv: readonly string[]): string | null {
  let found: string | null = null;
  for (let i = 0; i < argv.length; i += 1) {
    const arg = argv[i] ?? "";
    if (arg === "--port" || arg === "-p") {
      const next = argv[i + 1];
      if (next !== undefined && !next.startsWith("-")) found = next;
      continue;
    }
    const match = /^(?:--port|-p)=(.*)$/.exec(arg);
    if (match) found = match[1] ?? "";
  }
  return found;
}

/** The default the tower binds when nothing says otherwise. */
export const DEFAULT_BIND_HOST = "127.0.0.1";
export const DEFAULT_BIND_PORT = "8654";

/**
 * What the launcher is being asked to bind, from every source that can say so.
 *
 * Precedence, most explicit first: an argument on the command line, then
 * NOTE4C_TOWER_HOST, then loopback. An operator who wants the LAN has to say
 * so somewhere; nothing here infers it from an interface list.
 */
export function resolveRequestedBind(input: {
  argv: readonly string[];
  env: Record<string, string | undefined>;
}): { host: string; port: string; from: "argv" | "env" | "default" } {
  const argvHost = hostFromArgv(input.argv);
  const envHost = input.env.NOTE4C_TOWER_HOST?.trim();
  const port =
    portFromArgv(input.argv) ??
    (input.env.NOTE4C_TOWER_PORT?.trim() || DEFAULT_BIND_PORT);

  if (argvHost !== null) return { host: argvHost, port, from: "argv" };
  if (envHost !== undefined && envHost.length > 0) {
    return { host: envHost, port, from: "env" };
  }
  return { host: DEFAULT_BIND_HOST, port, from: "default" };
}
