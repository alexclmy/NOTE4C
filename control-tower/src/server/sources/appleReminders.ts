import { spawn } from "node:child_process";
import { z } from "zod";
import { DEFAULT_REMINDERS_LIST, type RemindersValue } from "@/core/render/data";
import type { ModuleData } from "@/core/render/types";
import { remindctlBin } from "@/server/config";
import { SourceError } from "./http";
import { notConfigured } from "./notConfigured";

/**
 * Apple Reminders, read only, through remindctl on this Mac.
 *
 * WHAT THIS IS ALLOWED TO DO
 * --------------------------
 * Read. Nothing else, ever. `remindctl` can add, edit, complete, delete and
 * rename reminders in iCloud, and none of those words appear in this file
 * except in the allowlist that refuses them. The NOTE4C is a display: it has
 * no buttons, nobody can confirm anything on it, and a panel that could
 * complete a reminder would be a write triggered by a render.
 *
 * That refusal is structural rather than a convention: every invocation goes
 * through `runRemindctl`, which throws on any subcommand that is not `list`,
 * `show` or `status`, and the process is spawned with no shell, no stdin and a
 * hard timeout.
 *
 * WHAT LEAVES THE SERVER
 * ----------------------
 * Counts. The adapter reads titles, due dates and priorities because it has to
 * sort and filter on them; `remindersValue()` projects all of that down to
 * openCount, overdueCount and the configured list name before anything crosses
 * the HTTP boundary into a browser. Reminder `notes` are never read at all:
 * the schema below does not declare the key, so zod strips it and no code path
 * in this repository can reach it.
 *
 * THE LIST IS RESOLVED BY NAME
 * ----------------------------
 * By exact title, and by nothing else. A list rebuilt in iCloud keeps its name
 * and loses its identifier, so an identifier would be the brittle key. Zero
 * matches and more than one match are both reported as unavailable with a
 * reason: guessing which of two lists sharing a name was meant is exactly the
 * kind of invention this product does not do.
 */

/**
 * The exact shared list. Configurable; this is only the default, and it is
 * defined in core so a module schema can offer it without importing anything
 * that spawns a process.
 */
export { DEFAULT_REMINDERS_LIST };

/**
 * The absolute path to `remindctl`, from NOTE4C_REMINDCTL_BIN.
 *
 * No PATH lookup, ever: this is spawned without a shell, and resolving a bare
 * name through the environment is how a different binary of the same name ends
 * up talking to somebody's Reminders. Empty means the source is not
 * configured, which is the default.
 */
export function remindctlPath(): string {
  return remindctlBin();
}

/** A Reminders read is a local IPC round trip; six seconds is generous. */
export const REMINDCTL_TIMEOUT_MS = 6_000;

/**
 * The only subcommands this tower may run.
 *
 * `status` reports whether the binary has Full Access and touches nothing.
 * `list` and `show` read. Everything else mutates iCloud.
 */
export const READ_ONLY_SUBCOMMANDS = ["list", "show", "status"] as const;

/**
 * Subcommands that must never be reachable. Listed explicitly so the refusal
 * is a named decision in the diff rather than an implication of the allowlist.
 */
export const FORBIDDEN_SUBCOMMANDS = [
  "add",
  "new",
  "create",
  "edit",
  "update",
  "set",
  "complete",
  "uncomplete",
  "done",
  "delete",
  "remove",
  "rm",
  "rename",
  "move",
  "import",
  "export",
] as const;

export class ReminderWriteRefusedError extends Error {
  constructor(argv: readonly string[]) {
    super(
      `Refusing to run remindctl ${argv[0] ?? "(no subcommand)"}: the tower is read-only toward Reminders`,
    );
    this.name = "ReminderWriteRefusedError";
  }
}

/**
 * Gate every invocation. Exported so a test can prove the refusal directly
 * rather than by observing that no write happened.
 */
export function assertReadOnlyArgs(argv: readonly string[]): void {
  const subcommand = argv[0];
  if (
    subcommand === undefined ||
    !(READ_ONLY_SUBCOMMANDS as readonly string[]).includes(subcommand)
  ) {
    throw new ReminderWriteRefusedError(argv);
  }
  for (const arg of argv) {
    if ((FORBIDDEN_SUBCOMMANDS as readonly string[]).includes(arg)) {
      throw new ReminderWriteRefusedError(argv);
    }
  }
}

export interface RemindctlResult {
  status: number | null;
  stdout: string;
  /** True when the process was killed by the timeout rather than exiting. */
  timedOut: boolean;
}

/**
 * Injected in tests. Production passes the spawning implementation below.
 *
 * The return type admits a plain value as well as a promise so that the unit
 * suite can keep driving this with ordinary synchronous fakes — a fixture that
 * returns a string does not need to be a coroutine to prove a parser — while
 * production is genuinely asynchronous. Callers `await` either.
 */
export type RemindctlRunner = (
  argv: readonly string[],
  timeoutMs: number,
) => RemindctlResult | Promise<RemindctlResult>;

/**
 * Spawn remindctl with no shell and no stdin.
 *
 * ASYNCHRONOUS, and that is the point of it.
 *
 * This used to be `spawnSync`. Node is single threaded, so a synchronous spawn
 * stops the entire server: for as long as `remindctl` takes to answer — twice,
 * once to index the lists and once to read one — nothing else in the process
 * runs. Not the other source adapters, not another browser tab, not a request
 * that has nothing to do with Reminders. With the six-second timeout below,
 * one wedged Reminders daemon froze the whole tower for twelve seconds, and
 * the Overview page asked for exactly that once a minute.
 *
 * stdin is /dev/null rather than inherited, so an interactive prompt can only
 * end in EOF: the adapter can never sit waiting for a human who is not there,
 * and cannot be talked into a confirmation.
 *
 * The timeout is enforced here rather than by `spawn`'s own option because a
 * killed child still has to be reaped and its streams drained; doing it
 * explicitly is what lets `timedOut` be reported as a fact rather than
 * inferred from an error code.
 */
export const spawnRemindctl: RemindctlRunner = (argv, timeoutMs) => {
  assertReadOnlyArgs(argv);
  const binary = remindctlPath();
  if (binary.length === 0) {
    throw new SourceError(
      notConfigured("Set NOTE4C_REMINDCTL_BIN to the absolute path of remindctl."),
    );
  }

  // A megabyte is far more than a household list; past that something is wrong
  // and truncating beats buffering without limit.
  const MAX_STDOUT_BYTES = 1024 * 1024;

  return new Promise<RemindctlResult>((resolve) => {
    const child = spawn(binary, [...argv], {
      stdio: ["ignore", "pipe", "pipe"],
      shell: false,
      windowsHide: true,
    });

    let stdout = "";
    let truncated = false;
    let timedOut = false;
    let settled = false;

    const timer = setTimeout(() => {
      timedOut = true;
      child.kill("SIGKILL");
    }, timeoutMs);

    const finish = (status: number | null): void => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      resolve({ status: truncated ? null : status, stdout, timedOut });
    };

    child.stdout?.setEncoding("utf8");
    child.stdout?.on("data", (chunk: string) => {
      if (truncated) return;
      stdout += chunk;
      if (stdout.length > MAX_STDOUT_BYTES) {
        truncated = true;
        stdout = "";
        child.kill("SIGKILL");
      }
    });
    // Drained and discarded. stderr from an unowned binary can carry a path or
    // a title, and nothing here has any use for it.
    child.stderr?.resume();

    child.on("error", () => finish(null));
    child.on("close", (code) => finish(code));
  });
};

/** The flags that make remindctl machine readable and never interactive. */
const JSON_FLAGS = ["--json", "--no-input", "--no-color"] as const;

export function listArgs(): string[] {
  return ["list", ...JSON_FLAGS];
}

export function showArgs(listName: string): string[] {
  return ["show", "all", "--list", listName, ...JSON_FLAGS];
}

/**
 * The measured shape of `remindctl list --json` on 0.1.1. Unknown keys are
 * stripped rather than refused, so a patch release that adds a field does not
 * take the source down.
 */
const ListEntrySchema = z.object({
  id: z.string(),
  title: z.string(),
  reminderCount: z.number().int().nonnegative().optional(),
  overdueCount: z.number().int().nonnegative().optional(),
});

/**
 * The measured shape of one reminder.
 *
 * `notes` is deliberately absent from this schema. z.object strips what it
 * does not declare, so private free text is dropped at the parse boundary and
 * cannot be reached by later code even by mistake.
 *
 * `priority` is a string enum in this build of remindctl, not a number.
 * `dueDate` and `completionDate` are absent keys rather than nulls when unset.
 */
const RawReminderSchema = z.object({
  id: z.string(),
  title: z.string(),
  isCompleted: z.boolean(),
  listName: z.string().optional(),
  listID: z.string().optional(),
  priority: z.enum(["none", "low", "medium", "high"]).optional(),
  dueDate: z.string().optional(),
});

export const REMINDER_PRIORITIES = ["high", "medium", "low", "none"] as const;
export type ReminderPriority = (typeof REMINDER_PRIORITIES)[number];

/** A reminder as the server keeps it. No notes, by construction. */
export interface ReminderRecord {
  id: string;
  title: string;
  /** ISO 8601 with offset, when the reminder has a due date at all. */
  dueAt?: string;
  priority: ReminderPriority;
}

export interface RemindersSnapshot {
  listName: string;
  /** Incomplete reminders only, deterministically sorted. */
  open: ReminderRecord[];
  overdueCount: number;
  /** When the command returned, which is the only freshness that exists here. */
  observedAt: string;
  /**
   * Set when the list index and the reminder dump disagree about how many are
   * open. Counts only: never a title.
   */
  countMismatch?: string;
}

function parseJson(raw: string, what: string): unknown {
  try {
    return JSON.parse(raw);
  } catch {
    throw new SourceError(`remindctl ${what} did not return JSON`);
  }
}

/**
 * Sort order, fixed so the same list always produces the same snapshot:
 * soonest due first, undated last, then higher priority, then title, then id.
 *
 * Titles are compared with localeCompare so an emoji-prefixed title sorts
 * stably rather than by UTF-16 accident.
 */
export function sortReminders(records: ReminderRecord[]): ReminderRecord[] {
  const rank = (priority: ReminderPriority): number =>
    REMINDER_PRIORITIES.indexOf(priority);
  return [...records].sort((a, b) => {
    if (a.dueAt !== b.dueAt) {
      if (a.dueAt === undefined) return 1;
      if (b.dueAt === undefined) return -1;
      return a.dueAt.localeCompare(b.dueAt);
    }
    if (a.priority !== b.priority) return rank(a.priority) - rank(b.priority);
    const byTitle = a.title.localeCompare(b.title);
    return byTitle !== 0 ? byTitle : a.id.localeCompare(b.id);
  });
}

export interface ReadRemindersOptions {
  listName?: string;
  now?: Date;
  run?: RemindctlRunner;
  timeoutMs?: number;
}

/**
 * Read the list. Throws SourceError with a content-free message on every
 * failure the caller has to tell a human about.
 *
 * Asynchronous because the spawn is: see `spawnRemindctl`. It awaits the
 * runner rather than requiring one, so an injected synchronous fixture still
 * works unchanged.
 */
export async function readRemindersSnapshot(
  options: ReadRemindersOptions = {},
): Promise<RemindersSnapshot> {
  const listName = options.listName ?? DEFAULT_REMINDERS_LIST;
  const now = options.now ?? new Date();
  const run = options.run ?? spawnRemindctl;
  const timeoutMs = options.timeoutMs ?? REMINDCTL_TIMEOUT_MS;

  const index = await run(listArgs(), timeoutMs);
  if (index.timedOut) throw new SourceError("remindctl timed out listing lists");
  if (index.status !== 0) {
    throw new SourceError(`remindctl list exited ${String(index.status)}`);
  }

  const lists = z
    .array(ListEntrySchema)
    .safeParse(parseJson(index.stdout, "list"));
  if (!lists.success) {
    throw new SourceError("remindctl list returned an unexpected shape");
  }

  // Exact title match, and no fallback. A near match is a different list.
  const matches = lists.data.filter((entry) => entry.title === listName);
  if (matches.length === 0) {
    throw new SourceError(
      `No Reminders list is named exactly as configured (${lists.data.length} lists were offered)`,
    );
  }
  if (matches.length > 1) {
    throw new SourceError(
      `${matches.length} Reminders lists share that exact name; rename one so the tower can tell them apart`,
    );
  }

  const dump = await run(showArgs(listName), timeoutMs);
  if (dump.timedOut) throw new SourceError("remindctl timed out reading the list");
  if (dump.status !== 0) {
    throw new SourceError(`remindctl show exited ${String(dump.status)}`);
  }

  const rows = z
    .array(RawReminderSchema)
    .safeParse(parseJson(dump.stdout, "show"));
  if (!rows.success) {
    throw new SourceError("remindctl show returned an unexpected shape");
  }

  // `show all` includes completed reminders, so the filter is ours to do.
  const open: ReminderRecord[] = [];
  for (const row of rows.data) {
    if (row.isCompleted) continue;
    // Defensive: the dump is supposed to be scoped to one list already.
    if (row.listName !== undefined && row.listName !== listName) continue;
    open.push({
      id: row.id,
      title: row.title,
      ...(row.dueDate !== undefined ? { dueAt: isoOrThrow(row.dueDate) } : {}),
      priority: row.priority ?? "none",
    });
  }

  const sorted = sortReminders(open);
  const overdueCount = sorted.filter(
    (record) =>
      record.dueAt !== undefined && new Date(record.dueAt).getTime() < now.getTime(),
  ).length;

  const declared = matches[0]?.reminderCount;
  const mismatch =
    declared !== undefined && declared !== sorted.length
      ? `the list index says ${declared} open, the dump says ${sorted.length}`
      : undefined;

  return {
    listName,
    open: sorted,
    overdueCount,
    observedAt: now.toISOString(),
    ...(mismatch ? { countMismatch: mismatch } : {}),
  };
}

function isoOrThrow(value: string): string {
  const date = new Date(value);
  if (Number.isNaN(date.getTime())) {
    throw new SourceError("A reminder carried an unreadable due date");
  }
  return date.toISOString();
}

/**
 * The browser-facing projection: counts and the configured list name.
 *
 * Three distinct states, as the rest of the renderer expects: unavailable when
 * the read failed, ok with openCount 0 for an empty list, ok with a count for a
 * populated one. An empty list is a fact, not a failure, and it is not a zero
 * standing in for a missing value.
 */
export async function remindersValue(
  options: ReadRemindersOptions = {},
): Promise<ModuleData<RemindersValue>> {
  try {
    const snapshot = await readRemindersSnapshot(options);
    return {
      state: "ok",
      value: {
        openCount: snapshot.open.length,
        overdueCount: snapshot.overdueCount,
        listName: snapshot.listName,
      },
      observedAt: snapshot.observedAt,
      ...(snapshot.countMismatch ? { detail: snapshot.countMismatch } : {}),
    };
  } catch (error) {
    return {
      state: "unavailable",
      detail:
        error instanceof SourceError || error instanceof ReminderWriteRefusedError
          ? error.message
          : "Reminders unavailable",
    };
  }
}

// ------------------------------------------------------------------ cache --

/**
 * How long a Reminders reading is reused. Five minutes.
 *
 * The number is set by what the panel is for, not by what the API can stand.
 * This is e-paper on a wall: the count of open items is refreshed when a frame
 * is pushed, and between pushes nobody is watching it change. Meanwhile the
 * Overview page polls once a minute, and before this cache every one of those
 * polls spawned `remindctl` twice — two processes a minute, forever, each one
 * touching somebody's whole iCloud Reminders database, to redraw a number that
 * had not moved.
 *
 * Five minutes is also comfortably shorter than the shortest sensible
 * dashboard refresh interval, so the scheduler never paints a frame from a
 * reading it could have refreshed.
 */
export const REMINDERS_TTL_MS = 5 * 60_000;

interface RemindersCacheEntry {
  listName: string;
  data: ModuleData<RemindersValue>;
  fetchedAtMs: number;
}

let remindersCache: RemindersCacheEntry | null = null;
/** One read at a time, per list. Concurrent callers share the same spawn. */
let remindersInFlight: { listName: string; work: Promise<ModuleData<RemindersValue>> } | null =
  null;

/** Reset module state between unit tests. Never used by production code. */
export function resetRemindersCacheForTests(): void {
  remindersCache = null;
  remindersInFlight = null;
}

export interface CachedRemindersOptions extends ReadRemindersOptions {
  /** Ignore the cache and spawn. The scheduler uses this before a push. */
  force?: boolean;
  ttlMs?: number;
}

/**
 * `remindersValue`, but at most once every `REMINDERS_TTL_MS` per list.
 *
 * Keyed on the list name because the name *is* the lookup key for this adapter
 * — a dashboard that binds a different list is asking a different question,
 * and answering it from the previous list's count would be inventing a number.
 *
 * A failure is cached too, and deliberately. Without that, a Reminders daemon
 * that is refusing to answer turns every single poll back into two six-second
 * spawns, which is the exact behaviour the cache exists to stop and is at its
 * worst precisely when things are already going wrong.
 */
export async function cachedRemindersValue(
  options: CachedRemindersOptions = {},
): Promise<ModuleData<RemindersValue>> {
  const listName = options.listName ?? DEFAULT_REMINDERS_LIST;
  const ttlMs = options.ttlMs ?? REMINDERS_TTL_MS;
  const nowMs = (options.now ?? new Date()).getTime();

  if (options.force !== true && remindersCache !== null) {
    const age = nowMs - remindersCache.fetchedAtMs;
    if (remindersCache.listName === listName && age >= 0 && age < ttlMs) {
      return remindersCache.data;
    }
  }

  if (remindersInFlight !== null && remindersInFlight.listName === listName) {
    return remindersInFlight.work;
  }

  const work = (async () => {
    try {
      const data = await remindersValue({ ...options, listName });
      remindersCache = { listName, data, fetchedAtMs: nowMs };
      return data;
    } finally {
      remindersInFlight = null;
    }
  })();
  remindersInFlight = { listName, work };
  return work;
}
