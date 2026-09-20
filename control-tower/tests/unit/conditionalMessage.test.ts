import { describe, expect, it } from "vitest";
import { DEFAULT_REMINDERS_LIST } from "@/core/render/data";
import { FrameBuffer, pack } from "@/core/frame";
import { BLACK, RED, WHITE } from "@/core/palette";
import {
  ConditionSchema,
  ConditionalMessageOptions,
  conditionalMessage,
  evaluateCondition,
  evaluateConditions,
  explainCondition,
  isConfigured,
  type Condition,
} from "@/core/render/modules/conditionalMessage";
import { cellsToPixels, type ModuleData, type RenderContext } from "@/core/render/types";
import type { LayoutNote } from "@/core/render/text";
import { sha256HexSync } from "@/server/hash";
import { sourceBindings } from "@/server/sources";
import { starterDashboard } from "@/core/model";
import { newModuleId } from "@/core/model";
import { fixtureSources, noSources } from "./fixtures/render";

const TORONTO = "America/Toronto";

/** 2026-09-11 is a Friday in Toronto. Every date case below leans on that. */
const FRIDAY = new Date("2026-09-11T13:00:00Z");

function ctx(overrides: Partial<RenderContext> = {}): RenderContext {
  return { now: FRIDAY, timeZone: TORONTO, sources: fixtureSources(), ...overrides };
}

function condition(raw: unknown): Condition {
  return ConditionSchema.parse(raw);
}

function render(
  options: Record<string, unknown>,
  context: RenderContext = ctx(),
  span = { w: 4, h: 1 },
): { frame: FrameBuffer; notes: LayoutNote[] } {
  const notes: LayoutNote[] = [];
  const frame = new FrameBuffer(WHITE);
  conditionalMessage.render(
    frame,
    cellsToPixels(0, 0, span.w, span.h),
    { state: "ok" } as ModuleData<never>,
    ConditionalMessageOptions.parse(options),
    {
      ...context,
      report: { overflow: () => undefined, note: (fact) => notes.push(fact) },
    },
  );
  return { frame, notes };
}

function digest(fb: FrameBuffer): string {
  return sha256HexSync(pack(fb));
}

function ink(fb: FrameBuffer, colour: number): number {
  let count = 0;
  for (const value of fb.pixels) if (value === colour) count += 1;
  return count;
}

describe("the condition schema", () => {
  it("accepts only the kinds it knows, and nothing that is code", () => {
    expect(ConditionSchema.safeParse({ kind: "dateRange" }).success).toBe(true);
    expect(ConditionSchema.safeParse({ kind: "daysOfWeek" }).success).toBe(true);
    expect(
      ConditionSchema.safeParse({ kind: "sourceState", source: "weather" })
        .success,
    ).toBe(true);
    expect(ConditionSchema.safeParse({ kind: "remindersCount" }).success).toBe(
      true,
    );
    // There is no expression kind, and there is no way to add one by hand.
    expect(
      ConditionSchema.safeParse({ kind: "expression", code: "1 === 1" }).success,
    ).toBe(false);
    expect(ConditionSchema.safeParse({ kind: "javascript" }).success).toBe(false);
  });

  it("refuses a date that is not a plain date", () => {
    expect(
      ConditionSchema.safeParse({ kind: "dateRange", from: "next tuesday" })
        .success,
    ).toBe(false);
    expect(
      ConditionSchema.safeParse({ kind: "dateRange", from: "2026-09-11" })
        .success,
    ).toBe(true);
  });

  it("refuses a day number that is not a day", () => {
    expect(
      ConditionSchema.safeParse({ kind: "daysOfWeek", days: [7] }).success,
    ).toBe(false);
    expect(
      ConditionSchema.safeParse({ kind: "daysOfWeek", days: [0, 6] }).success,
    ).toBe(true);
  });

  it("refuses a source nothing on this machine provides", () => {
    expect(
      ConditionSchema.safeParse({ kind: "sourceState", source: "stock_market" })
        .success,
    ).toBe(false);
  });

  it("explains every kind in a sentence a person can read", () => {
    const samples: unknown[] = [
      { kind: "dateRange", from: "2026-12-01", to: "2026-12-25" },
      { kind: "dateRange", from: "2026-12-01" },
      { kind: "dateRange" },
      { kind: "daysOfWeek", days: [1, 3] },
      { kind: "daysOfWeek" },
      { kind: "sourceState", source: "calendar", state: "unavailable" },
      { kind: "remindersCount", compare: "atLeast", value: 3 },
      { kind: "remindersCount", compare: "atMost", value: 0 },
    ];
    for (const sample of samples) {
      const explained = explainCondition(condition(sample));
      expect(explained.length).toBeGreaterThan(10);
      expect(explained).not.toMatch(/undefined|\[object/);
    }
  });

  it("knows when it has not been given enough to decide", () => {
    expect(isConfigured(condition({ kind: "dateRange" }))).toBe(false);
    expect(isConfigured(condition({ kind: "daysOfWeek" }))).toBe(false);
    expect(
      isConfigured(condition({ kind: "dateRange", from: "2026-01-01" })),
    ).toBe(true);
    expect(isConfigured(condition({ kind: "daysOfWeek", days: [5] }))).toBe(
      true,
    );
  });
});

describe("evaluating a condition", () => {
  it("date range is inclusive at both ends", () => {
    const between = condition({
      kind: "dateRange",
      from: "2026-09-11",
      to: "2026-09-11",
      timeZone: TORONTO,
    });
    expect(evaluateCondition(between, ctx()).value).toBe(true);
    expect(
      evaluateCondition(between, ctx({ now: new Date("2026-09-12T13:00:00Z") }))
        .value,
    ).toBe(false);
  });

  it("an open end means exactly that", () => {
    const from = condition({ kind: "dateRange", from: "2026-01-01" });
    const to = condition({ kind: "dateRange", to: "2026-01-01" });
    expect(evaluateCondition(from, ctx()).value).toBe(true);
    expect(evaluateCondition(to, ctx()).value).toBe(false);
  });

  it("reads the date in the timezone it was given, not the server's", () => {
    // 2026-09-12T02:00Z is still the 11th in Toronto and already the 12th in
    // Paris. The same instant, two different answers, both correct.
    const late = ctx({ now: new Date("2026-09-12T02:00:00Z") });
    const toronto = condition({
      kind: "dateRange",
      from: "2026-09-11",
      to: "2026-09-11",
      timeZone: TORONTO,
    });
    const paris = condition({
      kind: "dateRange",
      from: "2026-09-11",
      to: "2026-09-11",
      timeZone: "Europe/Paris",
    });
    expect(evaluateCondition(toronto, late).value).toBe(true);
    expect(evaluateCondition(paris, late).value).toBe(false);
  });

  it("days of the week counts Sunday as zero", () => {
    const friday = condition({ kind: "daysOfWeek", days: [5], timeZone: TORONTO });
    expect(evaluateCondition(friday, ctx()).value).toBe(true);
    expect(
      evaluateCondition(friday, ctx({ now: new Date("2026-09-12T13:00:00Z") }))
        .value,
    ).toBe(false);
    const saturday = condition({ kind: "daysOfWeek", days: [6], timeZone: TORONTO });
    expect(
      evaluateCondition(saturday, ctx({ now: new Date("2026-09-12T13:00:00Z") }))
        .value,
    ).toBe(true);
  });

  it("source state is true only when the state matches", () => {
    const calendarOk = condition({ kind: "sourceState", source: "calendar", state: "ok" });
    expect(evaluateCondition(calendarOk, ctx()).value).toBe(true);
    expect(
      evaluateCondition(calendarOk, ctx({ sources: noSources() })).value,
    ).toBe(false);

    const calendarDown = condition({
      kind: "sourceState",
      source: "calendar",
      state: "unavailable",
    });
    expect(
      evaluateCondition(calendarDown, ctx({ sources: noSources() })).value,
    ).toBe(true);
  });

  it("is false, not true, when no source data was collected at all", () => {
    const anything = condition({ kind: "sourceState", source: "reminders", state: "ok" });
    const outcome = evaluateCondition(anything, {
      now: FRIDAY,
      timeZone: TORONTO,
    });
    expect(outcome.value).toBe(false);
    expect(outcome.reason).toMatch(/No source data/);
  });

  it("compares the reminders count without ever reading a title", () => {
    const three = condition({ kind: "remindersCount", compare: "atLeast", value: 3 });
    const four = condition({ kind: "remindersCount", compare: "atLeast", value: 4 });
    const few = condition({ kind: "remindersCount", compare: "atMost", value: 3 });
    // The fixture says three are open.
    expect(evaluateCondition(three, ctx()).value).toBe(true);
    expect(evaluateCondition(four, ctx()).value).toBe(false);
    expect(evaluateCondition(few, ctx()).value).toBe(true);
  });

  it("is false with a reason when the reminders list could not be read", () => {
    const outcome = evaluateCondition(
      condition({ kind: "remindersCount", value: 1 }),
      ctx({ sources: noSources() }),
    );
    expect(outcome.value).toBe(false);
    expect(outcome.reason).toMatch(/could not be read/);
  });
});

describe("combining conditions", () => {
  const friday = { kind: "daysOfWeek", days: [5], timeZone: TORONTO };
  const saturday = { kind: "daysOfWeek", days: [6], timeZone: TORONTO };

  it("all means all", () => {
    expect(
      evaluateConditions([condition(friday), condition(saturday)], "all", ctx())
        .value,
    ).toBe(false);
    expect(
      evaluateConditions([condition(friday)], "all", ctx()).value,
    ).toBe(true);
  });

  it("any means any", () => {
    expect(
      evaluateConditions([condition(friday), condition(saturday)], "any", ctx())
        .value,
    ).toBe(true);
  });

  it("nothing configured is neither true nor false", () => {
    const verdict = evaluateConditions([], "all", ctx());
    expect(verdict.configured).toBe(false);
    expect(verdict.value).toBe(false);
  });

  it("ignores a condition that has not been filled in yet", () => {
    const verdict = evaluateConditions(
      [condition(friday), condition({ kind: "daysOfWeek" })],
      "all",
      ctx(),
    );
    expect(verdict.configured).toBe(true);
    expect(verdict.outcomes).toHaveLength(1);
    expect(verdict.value).toBe(true);
  });

  it("carries a reason for every condition it did evaluate", () => {
    const verdict = evaluateConditions([condition(friday)], "all", ctx());
    expect(verdict.outcomes[0]?.reason).toMatch(/Friday/);
    expect(verdict.outcomes[0]?.explain).toMatch(/Friday/);
  });
});

describe("conditional message rendering", () => {
  const FRIDAY_ONLY = [{ kind: "daysOfWeek", days: [5], timeZone: TORONTO }];

  it("renders a configuration state until a condition exists", () => {
    const { frame } = render({ body: { text: "Take the bins out" } });
    expect(ink(frame, RED)).toBeGreaterThan(50);
    // And it is NOT the message: an unconfigured tile must not just show it.
    const configured = render({
      body: { text: "Take the bins out" },
      conditions: FRIDAY_ONLY,
    });
    expect(digest(frame)).not.toBe(digest(configured.frame));
  });

  it("draws the message when the condition holds", () => {
    const { frame } = render({
      body: { text: "Take the bins out" },
      conditions: FRIDAY_ONLY,
    });
    expect(ink(frame, BLACK)).toBeGreaterThan(100);
  });

  it("draws nothing when it does not hold and hiding was chosen", () => {
    const { frame } = render(
      { body: { text: "Take the bins out" }, conditions: FRIDAY_ONLY },
      ctx({ now: new Date("2026-09-12T13:00:00Z") }),
    );
    expect(digest(frame)).toBe(digest(new FrameBuffer(WHITE)));
  });

  it("draws the fallback instead when that was chosen", () => {
    const { frame } = render(
      {
        body: { text: "Take the bins out" },
        fallback: { text: "Rien ce soir" },
        whenFalse: "showFallback",
        conditions: FRIDAY_ONLY,
      },
      ctx({ now: new Date("2026-09-12T13:00:00Z") }),
    );
    expect(ink(frame, RED)).toBeGreaterThan(20);
  });

  it("says which blank it is when a chosen role has no words", () => {
    const { notes } = render({ body: { text: "" }, conditions: FRIDAY_ONLY });
    const note = notes.find((entry) => entry.kind === "empty-role");
    expect(note?.detail).toMatch(/condition holds/);
  });

  it("draws its optional rule in the pigment asked for", () => {
    const plain = render({
      body: { text: "Note" },
      conditions: FRIDAY_ONLY,
    });
    const ruled = render({
      body: { text: "Note" },
      conditions: FRIDAY_ONLY,
      showRule: true,
      ruleColour: "accent",
    });
    expect(ink(ruled.frame, RED)).toBeGreaterThan(ink(plain.frame, RED));
  });

  it("is deterministic and packs to 30000 bytes in every state", () => {
    for (const context of [ctx(), ctx({ now: new Date("2026-09-12T13:00:00Z") })]) {
      const once = render(
        { body: { text: "Note" }, conditions: FRIDAY_ONLY },
        context,
      );
      const twice = render(
        { body: { text: "Note" }, conditions: FRIDAY_ONLY },
        context,
      );
      expect(digest(once.frame)).toBe(digest(twice.frame));
      expect(pack(once.frame)).toHaveLength(30000);
    }
  });

  it("never describes itself as a warning system", () => {
    expect(conditionalMessage.description).toMatch(/Not an alarm/);
    expect(conditionalMessage.description).toMatch(/only changes when it is pushed/);
  });
});

describe("what a conditional message makes the tower read", () => {
  function docWith(conditions: unknown[]) {
    const doc = starterDashboard("Kitchen panel");
    doc.modules = [
      {
        id: newModuleId(),
        type: "conditionalMessage",
        x: 0,
        y: 0,
        w: 4,
        h: 1,
        hidden: false,
        options: ConditionalMessageOptions.parse({
          conditions,
        }) as unknown as Record<string, unknown>,
      },
    ];
    return doc;
  }

  it("does not make the tower read Reminders unless a condition asks", () => {
    const bindings = sourceBindings(
      docWith([{ kind: "daysOfWeek", days: [5] }]),
    );
    expect(bindings.remindersList).toBeNull();
  });

  it("reads the configured list when a reminders condition asks", () => {
    const bindings = sourceBindings(
      docWith([{ kind: "remindersCount", value: 1 }]),
    );
    expect(bindings.remindersList).toBe(DEFAULT_REMINDERS_LIST);
  });

  it("also reads it for a plain source-state condition on reminders", () => {
    const bindings = sourceBindings(
      docWith([{ kind: "sourceState", source: "reminders", state: "ok" }]),
    );
    expect(bindings.remindersList).toBe(DEFAULT_REMINDERS_LIST);
  });

  it("honours a list name the module was given instead of the default", () => {
    const doc = docWith([{ kind: "remindersCount", value: 1 }]);
    const module = doc.modules[0];
    if (!module) throw new Error("no module");
    module.options = {
      ...module.options,
      remindersList: "A DIFFERENT LIST",
    };
    expect(sourceBindings(doc).remindersList).toBe("A DIFFERENT LIST");
  });
});
