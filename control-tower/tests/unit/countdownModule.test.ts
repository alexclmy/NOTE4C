import { describe, expect, it } from "vitest";
import { FrameBuffer, pack } from "@/core/frame";
import { BLACK, RED, WHITE } from "@/core/palette";
import {
  CountdownOptions,
  calendarDaysBetween,
  countdown,
  readCountdown,
  unitWord,
} from "@/core/render/modules/countdown";
import { cellsToPixels, type ModuleData } from "@/core/render/types";
import { sha256HexSync } from "@/server/hash";
import { starterDashboard } from "@/core/model";
import { renderDashboard, semanticHash } from "@/core/render";
import { moduleDefinition } from "@/core/render/modules";
import { fixtureSources } from "./fixtures/render";

const TORONTO = "America/Toronto";

function render(
  options: Record<string, unknown>,
  now: Date,
  span = { w: 3, h: 2 },
): FrameBuffer {
  const frame = new FrameBuffer(WHITE);
  countdown.render(
    frame,
    cellsToPixels(0, 0, span.w, span.h),
    { state: "ok" } as ModuleData<never>,
    CountdownOptions.parse(options),
    { now, timeZone: TORONTO },
  );
  return frame;
}

function digest(fb: FrameBuffer): string {
  return sha256HexSync(pack(fb));
}

function ink(fb: FrameBuffer, colour: number): number {
  let count = 0;
  for (const value of fb.pixels) if (value === colour) count += 1;
  return count;
}

function reading(options: Record<string, unknown>, now: string) {
  return readCountdown(CountdownOptions.parse(options), new Date(now));
}

describe("countdown never guesses a date", () => {
  it("starts with no target at all", () => {
    expect(CountdownOptions.parse({}).targetAt).toBeNull();
  });

  it("refuses a date with no offset, because that would need a guess", () => {
    expect(
      CountdownOptions.safeParse({ targetAt: "2026-12-25T08:00:00" }).success,
    ).toBe(false);
    expect(
      CountdownOptions.safeParse({ targetAt: "2026-12-25" }).success,
    ).toBe(false);
    expect(
      CountdownOptions.safeParse({ targetAt: "2026-12-25T08:00:00Z" }).success,
    ).toBe(true);
    expect(
      CountdownOptions.safeParse({ targetAt: "2026-12-25T08:00:00-05:00" })
        .success,
    ).toBe(true);
  });

  it("refuses a timezone this machine does not know", () => {
    const bad = CountdownOptions.safeParse({ timeZone: "Mars/Olympus" });
    expect(bad.success).toBe(false);
    if (!bad.success) {
      expect(bad.error.issues[0]?.message).toMatch(/IANA/);
    }
    expect(CountdownOptions.safeParse({ timeZone: "Europe/Paris" }).success).toBe(
      true,
    );
  });

  it("defaults to the panel's own timezone rather than the server's", () => {
    expect(CountdownOptions.parse({}).timeZone).toBe(TORONTO);
  });
});

describe("countdown arithmetic", () => {
  it("counts calendar days, so three days means three sleeps", () => {
    // 23:00 on the 11th to 01:00 on the 14th is 26 hours plus two midnights.
    const from = new Date("2026-09-12T03:00:00Z"); // 23:00 on the 11th, Toronto
    const to = new Date("2026-09-14T05:00:00Z"); // 01:00 on the 14th, Toronto
    expect(calendarDaysBetween(from, to, TORONTO)).toBe(3);
  });

  it("counts the same span differently in a different timezone, and says which", () => {
    const from = new Date("2026-09-12T03:00:00Z");
    const to = new Date("2026-09-14T05:00:00Z");
    // In UTC both instants are on the 12th and the 14th.
    expect(calendarDaysBetween(from, to, "UTC")).toBe(2);
  });

  it("is not fooled by a daylight saving change", () => {
    // Toronto leaves DST on 2026-11-01. Two civil days is still two days.
    const from = new Date("2026-10-31T16:00:00Z");
    const to = new Date("2026-11-02T17:00:00Z");
    expect(calendarDaysBetween(from, to, TORONTO)).toBe(2);
  });

  it("picks a unit that is never zero when the date is still ahead", () => {
    const target = "2026-09-11T12:00:00Z";
    expect(reading({ targetAt: target }, "2026-09-09T12:00:00Z")).toMatchObject({
      state: "ahead",
      unit: "days",
    });
    expect(reading({ targetAt: target }, "2026-09-11T06:00:00Z")).toMatchObject({
      state: "ahead",
      value: 6,
      unit: "hours",
    });
    const soon = reading({ targetAt: target }, "2026-09-11T11:59:30Z");
    expect(soon).toMatchObject({ state: "ahead", unit: "minutes" });
    // Thirty seconds left reads as one minute, never as zero.
    expect(soon.value).toBe(1);
  });

  it("honours an explicit unit instead of choosing one", () => {
    const target = "2026-09-13T12:00:00Z";
    expect(
      reading({ targetAt: target, unit: "hours" }, "2026-09-11T12:00:00Z").unit,
    ).toBe("hours");
    expect(
      reading({ targetAt: target, unit: "hours" }, "2026-09-11T12:00:00Z").value,
    ).toBe(48);
    expect(
      reading({ targetAt: target, unit: "minutes" }, "2026-09-11T12:00:00Z")
        .value,
    ).toBe(2880);
  });

  it("distinguishes no target from an unreadable one from a passed one", () => {
    expect(reading({}, "2026-09-11T12:00:00Z").state).toBe("unset");
    expect(
      readCountdown(
        { ...CountdownOptions.parse({}), targetAt: "not a date" },
        new Date("2026-09-11T12:00:00Z"),
      ).state,
    ).toBe("invalid");
    expect(
      reading({ targetAt: "2020-01-01T00:00:00Z" }, "2026-09-11T12:00:00Z")
        .state,
    ).toBe("passed");
  });

  it("writes the French plural itself", () => {
    expect(unitWord("days", 1)).toBe("jour");
    expect(unitWord("days", 2)).toBe("jours");
    expect(unitWord("hours", 1)).toBe("heure");
    expect(unitWord("minutes", 30)).toBe("minutes");
  });
});

describe("countdown rendering", () => {
  const NOW = new Date("2026-09-11T05:07:00.000Z");

  it("renders a configuration state, not a zero, with no target", () => {
    const frame = render({}, NOW);
    expect(ink(frame, RED)).toBeGreaterThan(50);
    // Nothing that could be read as a count.
    expect(digest(frame)).not.toBe(
      digest(render({ targetAt: "2026-09-11T06:00:00Z" }, NOW)),
    );
  });

  it("renders the same configuration state for an unreadable target", () => {
    const broken = new FrameBuffer(WHITE);
    countdown.render(
      broken,
      cellsToPixels(0, 0, 3, 2),
      { state: "ok" } as ModuleData<never>,
      { ...CountdownOptions.parse({}), targetAt: "yesterday" },
      { now: NOW, timeZone: TORONTO },
    );
    expect(digest(broken)).toBe(digest(render({}, NOW)));
  });

  it("is a pure function of the now it is handed", () => {
    const options = { targetAt: "2026-12-25T12:00:00Z" };
    expect(digest(render(options, NOW))).toBe(digest(render(options, NOW)));
    // A minute later is the same panel, because the unit is days.
    expect(digest(render(options, new Date(NOW.getTime() + 60_000)))).toBe(
      digest(render(options, NOW)),
    );
    // A day later is not.
    expect(
      digest(render(options, new Date(NOW.getTime() + 86_400_000))),
    ).not.toBe(digest(render(options, NOW)));
  });

  it("shows what it was told to show once the date has passed", () => {
    const passed = { targetAt: "2020-01-01T00:00:00Z" };
    const shown = render(passed, NOW);
    expect(ink(shown, RED)).toBeGreaterThan(20);

    const hidden = render({ ...passed, afterTarget: "hide" }, NOW);
    expect(ink(hidden, RED)).toBe(0);
    expect(ink(hidden, BLACK)).toBe(0);
  });

  it("puts the number in the pigment asked for", () => {
    const options = { targetAt: "2026-12-25T12:00:00Z" };
    const plain = render(options, NOW);
    const accented = render({ ...options, valueColour: "accent" }, NOW);
    expect(ink(accented, RED)).toBeGreaterThan(ink(plain, RED));
  });

  it("warns rather than cropping when the number does not fit", () => {
    const overflows: unknown[] = [];
    const frame = new FrameBuffer(WHITE);
    countdown.render(
      frame,
      cellsToPixels(0, 0, 2, 1),
      { state: "ok" } as ModuleData<never>,
      CountdownOptions.parse({
        targetAt: "2030-12-25T12:00:00Z",
        valueText: { style: { size: 34 } },
      }),
      {
        now: NOW,
        timeZone: TORONTO,
        report: { overflow: (fact) => overflows.push(fact) },
      },
    );
    expect(overflows.length).toBeGreaterThan(0);
    expect(pack(frame)).toHaveLength(30000);
  });

  it("packs to 30000 bytes in every state", () => {
    for (const options of [
      {},
      { targetAt: "2026-12-25T12:00:00Z" },
      { targetAt: "2020-01-01T00:00:00Z" },
      { targetAt: "2020-01-01T00:00:00Z", afterTarget: "hide" },
    ]) {
      expect(pack(render(options, NOW))).toHaveLength(30000);
    }
  });
});

describe("countdown and the duplicate-frame gate", () => {
  it("changes the semantic hash when the day changes, unlike the clock", async () => {
    const doc = starterDashboard("Kitchen panel");
    doc.modules = doc.modules.filter((module) => module.type === "timestamp");
    const morning = new Date("2026-09-11T13:00:00Z");
    const evening = new Date("2026-09-11T23:00:00Z");
    const tomorrow = new Date("2026-09-12T13:00:00Z");

    // A dashboard whose only moving part is the clock keeps its hash.
    expect(await semanticHash(doc, fixtureSources(), morning)).toBe(
      await semanticHash(doc, fixtureSources(), evening),
    );

    const withCountdown = {
      ...doc,
      modules: [
        ...doc.modules,
        {
          id: "cd",
          type: "countdown",
          x: 0,
          y: 0,
          w: 3,
          h: 2,
          hidden: false,
          options: moduleDefinition("countdown").schema.parse({
            targetAt: "2026-12-25T12:00:00Z",
          }) as Record<string, unknown>,
        },
      ],
    };
    const today = await semanticHash(withCountdown, fixtureSources(), morning);
    expect(await semanticHash(withCountdown, fixtureSources(), evening)).toBe(
      today,
    );
    // Tomorrow really is a different panel, and the gate has to know.
    expect(
      await semanticHash(withCountdown, fixtureSources(), tomorrow),
    ).not.toBe(today);
  });

  it("does not freeze under a semantic render, because its value is content", () => {
    const options = moduleDefinition("countdown").schema.parse({
      targetAt: "2026-12-25T12:00:00Z",
    });
    const doc = {
      ...starterDashboard("x"),
      modules: [
        {
          id: "cd",
          type: "countdown",
          x: 0,
          y: 0,
          w: 3,
          h: 2,
          hidden: false,
          options: options as Record<string, unknown>,
        },
      ],
    };
    const shown = renderDashboard(doc, fixtureSources(), {
      now: new Date("2026-09-11T13:00:00Z"),
      timeZone: TORONTO,
    });
    const hashed = renderDashboard(doc, fixtureSources(), {
      now: new Date("2026-09-11T13:00:00Z"),
      timeZone: TORONTO,
      semantic: true,
    });
    expect(digest(hashed)).toBe(digest(shown));
  });
});
