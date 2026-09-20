/**
 * The bind guard, run against this actual process.
 *
 * Split from `bindEnforcement.ts` so the policy and the observation stay pure
 * and unit testable, and everything that touches `process`, the clock and the
 * data root lives here in one small file.
 *
 * The check runs more than once. `instrumentation.register()` is called while
 * Next is still setting itself up, so on the first pass there is frequently no
 * listening socket to look at yet; a single early check would report "none"
 * forever and quietly stop being a guard. Re-checking a few seconds later is
 * what catches a real `next start --hostname 0.0.0.0`.
 */

import {
  enforceBind,
  observeBind,
  type BindEnforcement,
} from "./bindEnforcement";
import { passphraseIsSet } from "./passphrase";

/**
 * When the bind is re-examined, in milliseconds after start-up.
 *
 * Three passes. The first is free and catches a process that is already
 * listening; the others cover Next binding its socket after instrumentation
 * ran. Beyond a few seconds a server that has not bound is not going to.
 */
export const BIND_RECHECK_MS = [0, 1_500, 5_000] as const;

let latest: BindEnforcement | null = null;

/** The most recent verdict, for `GET /api/diagnostics`. Null before any check. */
export function lastBindEnforcement(): BindEnforcement | null {
  return latest;
}

/** Test seam: forget what was observed. */
export function resetBindEnforcementForTests(): void {
  latest = null;
}

function activeHandles(): readonly unknown[] {
  // Undocumented, and used deliberately: it is the only way to ask this
  // process what it is actually listening on without owning the server. A
  // runtime that does not offer it degrades to the argv and environment
  // sources, which is why `observeBind` reports which one answered.
  const get = (process as unknown as { _getActiveHandles?: () => unknown[] })
    ._getActiveHandles;
  if (typeof get !== "function") return [];
  try {
    return get.call(process);
  } catch {
    return [];
  }
}

function passphraseIsSetSafely(): boolean {
  try {
    return passphraseIsSet();
  } catch {
    // An unreadable data root is not a passphrase. Failing closed here means
    // the LAN branch is refused, which is the safe direction.
    return false;
  }
}

export interface ProcessBindOverrides {
  handles?: readonly unknown[];
  argv?: readonly string[];
  env?: Record<string, string | undefined>;
}

/**
 * Observe and judge this process once. Takes no action.
 *
 * The overrides exist for the unit suite, which has to be able to say "this
 * process is listening on 0.0.0.0" without a test actually opening a socket on
 * every interface of the machine running it.
 */
export function checkProcessBind(
  overrides: ProcessBindOverrides = {},
): BindEnforcement {
  const env = overrides.env ?? (process.env as Record<string, string | undefined>);
  const enforcement = enforceBind(
    observeBind({
      handles: overrides.handles ?? activeHandles(),
      argv: overrides.argv ?? process.argv,
      env,
    }),
    {
      passphraseSet: passphraseIsSetSafely(),
      allowLanFlag: env.NOTE4C_TOWER_ALLOW_LAN,
    },
  );
  // "undetermined" never overwrites a verdict that was actually determined:
  // a later pass that cannot see the socket has not learned that the earlier
  // one was wrong.
  if (latest === null || enforcement.verdict !== "undetermined") {
    latest = enforcement;
  }
  return enforcement;
}

export interface BindWatchOptions extends ProcessBindOverrides {
  /** Injected in tests. Production exits the process. */
  onRefused?: (enforcement: BindEnforcement) => void;
  /** Injected in tests. Production schedules unref'd timers. */
  schedule?: (fn: () => void, delayMs: number) => void;
  log?: (message: string) => void;
}

function defaultOnRefused(): void {
  // Exit rather than throw. A throw inside instrumentation is caught by Next,
  // logged as a warning, and the server goes on serving on the address the
  // guard just refused — which is worse than no guard, because the log line
  // reads like an enforcement that happened.
  process.exit(1);
}

/**
 * Run the check now and again shortly after, and stop serving if it refuses.
 *
 * Returns the first verdict so a caller can log it; the later passes act on
 * their own.
 */
export function watchProcessBind(options: BindWatchOptions = {}): BindEnforcement {
  const log = options.log ?? ((message: string) => console.error(message));
  const onRefused = options.onRefused ?? defaultOnRefused;
  const schedule =
    options.schedule ??
    ((fn: () => void, delayMs: number) => {
      const timer = setTimeout(fn, delayMs);
      timer.unref?.();
    });

  const act = (enforcement: BindEnforcement): void => {
    if (enforcement.verdict !== "refused") return;
    log("");
    log("NOTE4C control tower: refusing to serve.");
    for (const refusal of enforcement.refusals) log(`  ${refusal}`);
    log(
      "  Start the tower with `npm start` (or `npm run dev`), which checks the host before binding.",
    );
    log("");
    onRefused(enforcement);
  };

  const overrides: ProcessBindOverrides = {
    ...(options.handles !== undefined ? { handles: options.handles } : {}),
    ...(options.argv !== undefined ? { argv: options.argv } : {}),
    ...(options.env !== undefined ? { env: options.env } : {}),
  };

  const first = checkProcessBind(overrides);
  act(first);

  for (const delay of BIND_RECHECK_MS) {
    if (delay === 0) continue;
    schedule(() => {
      act(checkProcessBind(overrides));
    }, delay);
  }

  return first;
}
