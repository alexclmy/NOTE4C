import { describe, expect, it, vi } from "vitest";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { afterEach, beforeEach } from "vitest";
import {
  DEFAULT_REMINDERS_LIST,
  FORBIDDEN_SUBCOMMANDS,
  READ_ONLY_SUBCOMMANDS,
  ReminderWriteRefusedError,
  assertReadOnlyArgs,
  listArgs,
  REMINDERS_TTL_MS,
  cachedRemindersValue,
  readRemindersSnapshot,
  spawnRemindctl,
  remindersValue,
  resetRemindersCacheForTests,
  showArgs,
  sortReminders,
  type RemindctlRunner,
  type ReminderRecord,
} from "@/server/sources/appleReminders";

/**
 * Every fixture here is invented. Nothing in this file has ever been in
 * anybody's Reminders, and nothing in this file talks to Reminders: the runner
 * is injected, the real one is never constructed, and a test that reached
 * iCloud would be a test that changed somebody's household.
 *
 * The content is deliberately generic — a list called "Chores", errands anyone
 * might have — rather than the household this project grew out of. A fixture
 * that names one family's routine is a fixture that ends up in a screenshot,
 * a failure message, or a public repository.
 *
 * The titles carry emoji and accented letters on purpose: a real list has
 * both, and a JSON path that mangles either would otherwise only be found on
 * the panel.
 */

const LIST = DEFAULT_REMINDERS_LIST;
const NOW = new Date("2026-09-11T13:00:00.000Z");

const LISTS = [
  { id: "L-1", title: "Inbox", reminderCount: 2, overdueCount: 0 },
  { id: "L-2", title: LIST, reminderCount: 3, overdueCount: 1 },
  { id: "L-3", title: "🎒 Schöol run", reminderCount: 0, overdueCount: 0 },
];

const REMINDERS = [
  {
    id: "R-1",
    title: "🥚 Buy eggs",
    isCompleted: false,
    listID: "L-2",
    listName: LIST,
    priority: "high",
    dueDate: "2026-09-10T08:00:00Z",
    notes: "SYNTHETIC NOTE THAT MUST NEVER LEAVE THE ADAPTER",
  },
  {
    id: "R-2",
    title: "🧹 Sweep the porch",
    isCompleted: false,
    listID: "L-2",
    listName: LIST,
    priority: "none",
  },
  {
    id: "R-3",
    title: "🍁 Bring in the planters",
    isCompleted: false,
    listID: "L-2",
    listName: LIST,
    priority: "medium",
    dueDate: "2026-09-20T12:00:00Z",
  },
  {
    id: "R-4",
    title: "Already done",
    isCompleted: true,
    listID: "L-2",
    listName: LIST,
    priority: "none",
    completionDate: "2026-09-01T10:00:00Z",
  },
];

/** A runner that answers from fixtures and records what it was asked. */
function fakeRunner(
  overrides: {
    lists?: unknown;
    reminders?: unknown;
    listStatus?: number | null;
    showStatus?: number | null;
    timedOut?: boolean;
    listRaw?: string;
    showRaw?: string;
  } = {},
): RemindctlRunner & { calls: string[][] } {
  const calls: string[][] = [];
  const run: RemindctlRunner = (argv) => {
    // The production guard, applied here too: a test double must not be a way
    // around the one rule this adapter has.
    assertReadOnlyArgs(argv);
    calls.push([...argv]);
    if (argv[0] === "list") {
      return {
        status: overrides.listStatus ?? 0,
        stdout:
          overrides.listRaw ?? JSON.stringify(overrides.lists ?? LISTS),
        timedOut: overrides.timedOut ?? false,
      };
    }
    return {
      status: overrides.showStatus ?? 0,
      stdout:
        overrides.showRaw ?? JSON.stringify(overrides.reminders ?? REMINDERS),
      timedOut: false,
    };
  };
  return Object.assign(run, { calls });
}

describe("the adapter cannot write", () => {
  it("allows only list, show and status", async () => {
    expect([...READ_ONLY_SUBCOMMANDS]).toEqual(["list", "show", "status"]);
    for (const subcommand of READ_ONLY_SUBCOMMANDS) {
      expect(() => assertReadOnlyArgs([subcommand])).not.toThrow();
    }
  });

  it("refuses every subcommand that would change iCloud", async () => {
    for (const subcommand of FORBIDDEN_SUBCOMMANDS) {
      expect(() => assertReadOnlyArgs([subcommand, LIST])).toThrow(
        ReminderWriteRefusedError,
      );
    }
  });

  it("refuses a mutating word smuggled in as an argument", async () => {
    expect(() => assertReadOnlyArgs(["show", "all", "--list", "complete"])).toThrow(
      ReminderWriteRefusedError,
    );
  });

  it("refuses an empty command rather than running a default", async () => {
    expect(() => assertReadOnlyArgs([])).toThrow(ReminderWriteRefusedError);
  });

  it("builds only read-only arguments, and never asks for input", async () => {
    for (const argv of [listArgs(), showArgs(LIST)]) {
      expect(() => assertReadOnlyArgs(argv)).not.toThrow();
      expect(argv).toContain("--json");
      expect(argv).toContain("--no-input");
      expect(argv).toContain("--no-color");
    }
  });

  it("only ever runs the two reads it needs", async () => {
    const run = fakeRunner();
    await readRemindersSnapshot({ run, now: NOW });
    expect(run.calls.map((call) => call[0])).toEqual(["list", "show"]);
  });
});

describe("resolving the list", () => {
  it("finds it by exact name, not by identifier", async () => {
    const run = fakeRunner();
    const snapshot = await readRemindersSnapshot({ run, now: NOW });
    expect(snapshot.listName).toBe(LIST);
    expect(run.calls[1]).toEqual(showArgs(LIST));
    // The identifier is never used as a lookup key.
    expect(JSON.stringify(run.calls)).not.toContain("L-2");
  });

  it("fails honestly when no list has that name", async () => {
    const value = await remindersValue({
      run: fakeRunner({ lists: [{ id: "L-1", title: "Inbox" }] }),
      now: NOW,
    });
    expect(value.state).toBe("unavailable");
    expect(value.detail).toMatch(/named exactly as configured/);
  });

  it("fails honestly rather than guessing between two of the same name", async () => {
    const value = await remindersValue({
      run: fakeRunner({
        lists: [
          { id: "L-2", title: LIST },
          { id: "L-9", title: LIST },
        ],
      }),
      now: NOW,
    });
    expect(value.state).toBe("unavailable");
    expect(value.detail).toMatch(/share that exact name/);
  });

  it("does not match a name that merely looks similar", async () => {
    const value = await remindersValue({
      run: fakeRunner({ lists: [{ id: "L-2", title: "Household list" }] }),
      now: NOW,
    });
    expect(value.state).toBe("unavailable");
  });

  it("reads whichever list it was configured for", async () => {
    const run = fakeRunner({
      lists: [{ id: "L-7", title: "🧺 Laundry" }],
      reminders: [],
    });
    const snapshot = await readRemindersSnapshot({
      run,
      now: NOW,
      listName: "🧺 Laundry",
    });
    expect(snapshot.listName).toBe("🧺 Laundry");
  });
});

describe("normalising what comes back", () => {
  it("keeps only incomplete reminders", async () => {
    const snapshot = await readRemindersSnapshot({ run: fakeRunner(), now: NOW });
    expect(snapshot.open.map((row) => row.id)).not.toContain("R-4");
    expect(snapshot.open).toHaveLength(3);
  });

  it("never carries a reminder's notes, at any depth", async () => {
    const snapshot = await readRemindersSnapshot({ run: fakeRunner(), now: NOW });
    const serialised = JSON.stringify(snapshot);
    expect(serialised).not.toContain("notes");
    expect(serialised).not.toContain("MUST NEVER LEAVE");
    for (const record of snapshot.open) {
      expect(Object.keys(record).sort()).not.toContain("notes");
    }
  });

  it("keeps emoji in a title byte for byte", async () => {
    const snapshot = await readRemindersSnapshot({ run: fakeRunner(), now: NOW });
    expect(snapshot.open.map((row) => row.title)).toContain(
      "🥚 Buy eggs",
    );
  });

  it("keeps a due date as an instant, and leaves the key off when there is none", async () => {
    const snapshot = await readRemindersSnapshot({ run: fakeRunner(), now: NOW });
    const dated = snapshot.open.find((row) => row.id === "R-1");
    const undated = snapshot.open.find((row) => row.id === "R-2");
    expect(dated?.dueAt).toBe("2026-09-10T08:00:00.000Z");
    expect(undated && "dueAt" in undated).toBe(false);
  });

  it("reads priority as the string enum this build of remindctl emits", async () => {
    const snapshot = await readRemindersSnapshot({ run: fakeRunner(), now: NOW });
    expect(snapshot.open.map((row) => row.priority)).toEqual([
      "high",
      "medium",
      "none",
    ]);
  });

  it("sorts deterministically: due first, undated last", async () => {
    const snapshot = await readRemindersSnapshot({ run: fakeRunner(), now: NOW });
    expect(snapshot.open.map((row) => row.id)).toEqual(["R-1", "R-3", "R-2"]);
    // And the same input always gives the same order.
    expect(
      (await readRemindersSnapshot({ run: fakeRunner(), now: NOW })).open.map(
        (row) => row.id,
      ),
    ).toEqual(["R-1", "R-3", "R-2"]);
  });

  it("breaks a tie by priority, then by title, then by id", async () => {
    const rows: ReminderRecord[] = [
      { id: "b", title: "Même", priority: "none" },
      { id: "a", title: "Même", priority: "none" },
      { id: "c", title: "Autre", priority: "high" },
    ];
    expect(sortReminders(rows).map((row) => row.id)).toEqual(["c", "a", "b"]);
  });

  it("counts an overdue reminder against the clock it was given", async () => {
    const snapshot = await readRemindersSnapshot({ run: fakeRunner(), now: NOW });
    expect(snapshot.overdueCount).toBe(1);
    const earlier = await readRemindersSnapshot({
      run: fakeRunner(),
      now: new Date("2026-09-01T00:00:00Z"),
    });
    expect(earlier.overdueCount).toBe(0);
  });

  it("records when it looked, which is the only freshness there is", async () => {
    const snapshot = await readRemindersSnapshot({ run: fakeRunner(), now: NOW });
    expect(snapshot.observedAt).toBe(NOW.toISOString());
  });

  it("notes a disagreement between the index and the dump, in counts only", async () => {
    const snapshot = await readRemindersSnapshot({
      run: fakeRunner({
        lists: [{ id: "L-2", title: LIST, reminderCount: 9 }],
      }),
      now: NOW,
    });
    expect(snapshot.countMismatch).toBe(
      "the list index says 9 open, the dump says 3",
    );
    expect(snapshot.countMismatch).not.toContain("eggs");
  });
});

describe("the three states the renderer sees", () => {
  it("populated: a count and nothing else", async () => {
    const value = await remindersValue({ run: fakeRunner(), now: NOW });
    expect(value.state).toBe("ok");
    expect(value.value).toEqual({
      openCount: 3,
      overdueCount: 1,
      listName: LIST,
    });
  });

  it("empty: ok with a zero that means zero, not a missing number", async () => {
    const value = await remindersValue({
      run: fakeRunner({
        lists: [{ id: "L-2", title: LIST, reminderCount: 0 }],
        reminders: [],
      }),
      now: NOW,
    });
    expect(value.state).toBe("ok");
    expect(value.value?.openCount).toBe(0);
  });

  it("unavailable: a timeout is not an empty list", async () => {
    const value = await remindersValue({ run: fakeRunner({ timedOut: true }), now: NOW });
    expect(value.state).toBe("unavailable");
    expect(value.detail).toMatch(/timed out/);
    expect(value.value).toBeUndefined();
  });

  it("unavailable: a non-zero exit is not an empty list", async () => {
    expect(
      (await remindersValue({ run: fakeRunner({ listStatus: 1 }), now: NOW })).state,
    ).toBe("unavailable");
    expect(
      (await remindersValue({ run: fakeRunner({ showStatus: 2 }), now: NOW })).state,
    ).toBe("unavailable");
  });

  it("unavailable: output that is not JSON", async () => {
    const value = await remindersValue({
      run: fakeRunner({ listRaw: "remindctl: permission denied" }),
      now: NOW,
    });
    expect(value.state).toBe("unavailable");
    expect(value.detail).toMatch(/did not return JSON/);
  });

  it("unavailable: JSON of the wrong shape", async () => {
    const value = await remindersValue({
      run: fakeRunner({ showRaw: JSON.stringify({ reminders: "lots" }) }),
      now: NOW,
    });
    expect(value.state).toBe("unavailable");
    expect(value.detail).toMatch(/unexpected shape/);
  });

  it("never puts a reminder's words in the detail a browser can read", async () => {
    for (const options of [
      { run: fakeRunner({ timedOut: true }) },
      { run: fakeRunner({ listStatus: 1 }) },
      { run: fakeRunner({ showRaw: "{}" }) },
      { run: fakeRunner() },
    ]) {
      const value = await remindersValue({ ...options, now: NOW });
      const detail = value.detail ?? "";
      expect(detail).not.toContain("eggs");
      expect(detail).not.toContain("porch");
      expect(detail).not.toContain("MUST NEVER LEAVE");
    }
  });

  it("absorbs an unexpected throw instead of taking the render down", async () => {
    const value = await remindersValue({
      run: (() => {
        throw new TypeError("spawn failed");
      }) as RemindctlRunner,
      now: NOW,
    });
    expect(value.state).toBe("unavailable");
    expect(value.detail).toBe("Reminders unavailable");
  });

  it("does not log anything, ever", async () => {
    const log = vi.spyOn(console, "log").mockImplementation(() => undefined);
    const warn = vi.spyOn(console, "warn").mockImplementation(() => undefined);
    const error = vi.spyOn(console, "error").mockImplementation(() => undefined);
    await remindersValue({ run: fakeRunner(), now: NOW });
    await remindersValue({ run: fakeRunner({ listStatus: 1 }), now: NOW });
    expect(log).not.toHaveBeenCalled();
    expect(warn).not.toHaveBeenCalled();
    expect(error).not.toHaveBeenCalled();
    log.mockRestore();
    warn.mockRestore();
    error.mockRestore();
  });
});


/**
 * The cache, which did not exist.
 *
 * Before this, every caller of `collectSources` spawned `remindctl` twice.
 * The Overview page polls once a minute, so a tab left open on a wall-mounted
 * laptop meant a hundred and twenty processes an hour, each one reaching into
 * somebody's entire iCloud Reminders database, to redraw a count that changes
 * a handful of times a day. Every test here fails against that code.
 */
describe("the Reminders read is cached", () => {
  beforeEach(() => {
    resetRemindersCacheForTests();
  });

  afterEach(() => {
    resetRemindersCacheForTests();
  });

  it("spawns once and answers the rest from the cache", async () => {
    const run = fakeRunner();
    for (let i = 0; i < 5; i += 1) {
      const value = await cachedRemindersValue({ run, listName: LIST, now: NOW });
      expect(value.state).toBe("ok");
    }
    // Two invocations: one `list`, one `show`. Not ten.
    expect(run.calls).toHaveLength(2);
  });

  it("spawns again once the TTL has run out", async () => {
    const run = fakeRunner();
    await cachedRemindersValue({ run, listName: LIST, now: NOW });
    await cachedRemindersValue({
      run,
      listName: LIST,
      now: new Date(NOW.getTime() + REMINDERS_TTL_MS - 1),
    });
    expect(run.calls).toHaveLength(2);

    await cachedRemindersValue({
      run,
      listName: LIST,
      now: new Date(NOW.getTime() + REMINDERS_TTL_MS + 1),
    });
    expect(run.calls).toHaveLength(4);
  });

  it("never answers about one list with another list's count", async () => {
    // The list name is the lookup key for this adapter, so two dashboards
    // bound to two lists are asking two different questions. Serving the
    // second from the first one's cache would be inventing a number.
    const run = fakeRunner({
      lists: [
        { id: "L-2", title: LIST, reminderCount: 3 },
        { id: "L-7", title: "🧺 Laundry", reminderCount: 0 },
      ],
      reminders: [],
    });
    await cachedRemindersValue({ run, listName: LIST, now: NOW });
    const other = await cachedRemindersValue({
      run,
      listName: "🧺 Laundry",
      now: NOW,
    });
    expect(other.value?.listName).toBe("🧺 Laundry");
    expect(run.calls).toHaveLength(4);
  });

  it("collapses concurrent reads into one spawn", async () => {
    const run = fakeRunner();
    const results = await Promise.all([
      cachedRemindersValue({ run, listName: LIST, now: NOW }),
      cachedRemindersValue({ run, listName: LIST, now: NOW }),
      cachedRemindersValue({ run, listName: LIST, now: NOW }),
    ]);
    expect(results.every((value) => value.state === "ok")).toBe(true);
    expect(run.calls).toHaveLength(2);
  });

  it("caches a failure too, so a broken daemon is not polled every minute", async () => {
    // The case this matters most in: `remindctl` timing out costs six seconds
    // per invocation. Without caching the failure, a Reminders daemon that has
    // wedged turns each poll back into twelve seconds of spawning — at exactly
    // the moment everything is already going wrong.
    const run = fakeRunner({ timedOut: true });
    const first = await cachedRemindersValue({ run, listName: LIST, now: NOW });
    expect(first.state).toBe("unavailable");
    await cachedRemindersValue({ run, listName: LIST, now: NOW });
    await cachedRemindersValue({ run, listName: LIST, now: NOW });
    expect(run.calls).toHaveLength(1);
  });

  it("spawns when the caller forces it, because a push must not paint a stale count", async () => {
    const run = fakeRunner();
    await cachedRemindersValue({ run, listName: LIST, now: NOW });
    await cachedRemindersValue({ run, listName: LIST, now: NOW, force: true });
    expect(run.calls).toHaveLength(4);
  });
});


/**
 * The spawn itself, which used to be synchronous.
 *
 * `spawnSync` stops the whole process. Node has one thread, so for however
 * long `remindctl` takes — up to the six-second timeout, twice per read —
 * nothing else in the server runs: not another request, not another source
 * adapter, not the scheduler. A page that polled once a minute asked for
 * exactly that.
 *
 * The binary here is `/bin/echo`, which is not remindctl and is not expected
 * to be: what is under test is the spawning, not the parsing, and echoing the
 * argv back is the cheapest way to prove a real child process ran.
 */
describe("the spawn does not block the event loop", () => {
  const previous = process.env.NOTE4C_REMINDCTL_BIN;

  beforeEach(() => {
    process.env.NOTE4C_REMINDCTL_BIN = "/bin/echo";
  });

  afterEach(() => {
    if (previous === undefined) delete process.env.NOTE4C_REMINDCTL_BIN;
    else process.env.NOTE4C_REMINDCTL_BIN = previous;
  });

  it("returns a promise rather than a finished result", async () => {
    const pending = spawnRemindctl(listArgs(), 5_000);
    // The load-bearing assertion. A synchronous runner returns a plain object
    // here, and `.then` is undefined.
    expect(typeof (pending as Promise<unknown>).then).toBe("function");

    // And the loop really is free while the child runs: a timer scheduled
    // after the spawn gets to fire. Under spawnSync it could not even have
    // been scheduled until the child had already exited.
    let timerFired = false;
    await new Promise<void>((resolve) =>
      setTimeout(() => {
        timerFired = true;
        resolve();
      }, 0),
    );
    expect(timerFired).toBe(true);

    const result = await pending;
    expect(result.timedOut).toBe(false);
    expect(result.stdout).toContain("list");
  });

  it("still refuses a mutating subcommand before any process exists", () => {
    expect(() => spawnRemindctl(["delete", LIST], 5_000)).toThrow(
      ReminderWriteRefusedError,
    );
  });

  it("reports a timeout as a timeout rather than as an exit code", async () => {
    // A stand-in that will not answer inside the budget, which is the only
    // property this test needs. Written to a temp file because the argv the
    // adapter sends always begins with an allowed subcommand, so a program
    // that takes a duration as its first argument cannot be used directly.
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), "note4c-remindctl-"));
    const script = path.join(dir, "slow");
    fs.writeFileSync(script, "#!/bin/sh\nsleep 5\n", { mode: 0o755 });
    process.env.NOTE4C_REMINDCTL_BIN = script;
    try {
      const started = Date.now();
      const result = await spawnRemindctl(listArgs(), 100);
      expect(result.timedOut).toBe(true);
      // Killed, not waited out: the whole point of the budget.
      expect(Date.now() - started).toBeLessThan(4_000);
    } finally {
      fs.rmSync(dir, { recursive: true, force: true });
    }
  });
});
