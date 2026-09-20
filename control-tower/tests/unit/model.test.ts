import { describe, expect, it } from "vitest";
import {
  DASHBOARD_SCHEMA_VERSION,
  applyDrag,
  DashboardValidationError,
  assertValidDashboard,
  emptyDashboard,
  findFreeSlot,
  layoutProblems,
  newDashboardId,
  newModuleId,
  normalizeDashboard,
  overlaps,
  parseDashboard,
  starterDashboard,
  type DashboardDoc,
  type ModuleInstance,
} from "@/core/model";
import { GRID_COLS, GRID_ROWS } from "@/core/render/types";
import { DEFAULT_THEME } from "@/core/theme";
import { moduleDefinition } from "@/core/render/modules";
import type { TextElement } from "@/core/render/text";

function doc(modules: ModuleInstance[]): DashboardDoc {
  return {
    schema_version: DASHBOARD_SCHEMA_VERSION,
    id: "d_test",
    title: "Test",
    status: "active",
    grid: { cols: GRID_COLS, rows: GRID_ROWS },
    theme: DEFAULT_THEME,
    refreshIntervalMinutes: null,
    modules,
    createdAt: "2026-09-11T00:00:00.000Z",
    updatedAt: "2026-09-11T00:00:00.000Z",
  };
}

function mod(
  id: string,
  type: string,
  x: number,
  y: number,
  w: number,
  h: number,
  options: Record<string, unknown> = {},
): ModuleInstance {
  return { id, type, x, y, w, h, hidden: false, options };
}

describe("grid", () => {
  it("is 8 columns by 6 rows of 50 px cells", () => {
    expect(GRID_COLS).toBe(8);
    expect(GRID_ROWS).toBe(6);
    expect(GRID_COLS * 50).toBe(400);
    expect(GRID_ROWS * 50).toBe(300);
  });
});

describe("layout validation", () => {
  it("accepts the starter dashboard", () => {
    expect(layoutProblems(starterDashboard("Kitchen panel"))).toEqual([]);
  });

  it("accepts an empty dashboard", () => {
    expect(layoutProblems(emptyDashboard("Blank"))).toEqual([]);
  });

  it("rejects a module that runs past the right edge", () => {
    const problems = layoutProblems(
      doc([mod("a", "haSensor", 6, 0, 3, 1)]),
    );
    expect(problems).toHaveLength(1);
    expect(problems[0]).toMatch(/runs past the 8x6 grid/);
  });

  it("rejects a module that runs past the bottom edge", () => {
    const problems = layoutProblems(doc([mod("a", "octopus", 0, 5, 2, 2)]));
    expect(problems[0]).toMatch(/runs past the 8x6 grid/);
  });

  it("rejects overlapping modules", () => {
    const problems = layoutProblems(
      doc([mod("a", "haSensor", 0, 0, 3, 1), mod("b", "haSensor", 2, 0, 3, 1)]),
    );
    expect(problems).toContain('Modules "a" and "b" overlap');
  });

  it("allows modules that only touch edges", () => {
    expect(
      layoutProblems(
        doc([
          mod("a", "haSensor", 0, 0, 3, 1),
          mod("b", "haSensor", 3, 0, 3, 1),
          mod("c", "haSensor", 0, 1, 3, 1),
        ]),
      ),
    ).toEqual([]);
  });

  it("rejects an unknown module type", () => {
    const problems = layoutProblems(doc([mod("a", "teleporter", 0, 0, 2, 1)]));
    expect(problems[0]).toMatch(/Unknown module type "teleporter"/);
  });

  it("rejects a duplicate module id", () => {
    const problems = layoutProblems(
      doc([mod("a", "haSensor", 0, 0, 3, 1), mod("a", "haSensor", 3, 0, 3, 1)]),
    );
    expect(problems).toContain('Duplicate module id "a"');
  });

  it("rejects a module smaller than its minimum span", () => {
    const problems = layoutProblems(doc([mod("a", "octopus", 0, 0, 1, 1)]));
    expect(problems.some((p) => /smaller than octopus allows/.test(p))).toBe(
      true,
    );
  });

  it("rejects a module larger than its maximum span", () => {
    const problems = layoutProblems(doc([mod("a", "timestamp", 0, 0, 8, 1)]));
    expect(problems.some((p) => /larger than timestamp allows/.test(p))).toBe(
      true,
    );
  });

  it("rejects options its module schema refuses", () => {
    const problems = layoutProblems(
      doc([
        mod("a", "haSensor", 0, 0, 3, 1, {
          entityId: "light.kitchen",
          label: "Lumière",
        }),
      ]),
    );
    expect(
      problems.some((p) =>
        /Only sensor\. and binary_sensor\. entities can be displayed/.test(p),
      ),
    ).toBe(true);
  });

  it("accepts a read-only sensor entity", () => {
    expect(
      layoutProblems(
        doc([
          mod("a", "haSensor", 0, 0, 3, 1, {
            entityId: "binary_sensor.front_door",
            label: { text: "Porte" },
          }),
        ]),
      ),
    ).toEqual([]);
  });

  it("reports every problem at once rather than the first", () => {
    const problems = layoutProblems(
      doc([
        mod("a", "haSensor", 6, 0, 3, 1),
        mod("b", "teleporter", 0, 0, 2, 1),
        mod("c", "haSensor", 0, 0, 3, 1),
      ]),
    );
    expect(problems.length).toBeGreaterThanOrEqual(3);
  });
});

describe("assertValidDashboard", () => {
  it("returns the document when it is valid", () => {
    const valid = starterDashboard("Kitchen panel");
    expect(assertValidDashboard(valid)).toBe(valid);
  });

  it("throws with every problem attached", () => {
    try {
      assertValidDashboard(doc([mod("a", "haSensor", 7, 0, 3, 1)]));
      expect.unreachable("should have thrown");
    } catch (error) {
      expect(error).toBeInstanceOf(DashboardValidationError);
      expect((error as DashboardValidationError).problems).toHaveLength(1);
    }
  });
});

describe("parseDashboard", () => {
  it("round-trips a serialised dashboard", () => {
    const original = starterDashboard("Kitchen panel");
    const restored = parseDashboard(JSON.parse(JSON.stringify(original)));
    expect(restored).toEqual(original);
  });

  it("rejects a v1 bare string where v2 expects a text element", () => {
    // The whole point of the v2 shape is that it is a different shape.
    // Anything still carrying v1 options has to go through the migration.
    const problems = layoutProblems(
      doc([mod("a", "haSensor", 0, 0, 3, 1, { label: "Porte" })]),
    );
    expect(problems).toHaveLength(1);
    expect(problems[0]).toMatch(/options rejected/);
  });

  it("refuses a document from a future schema version", () => {
    // 5 is the current version now; a future one is 6.
    const raw = { ...starterDashboard("x"), schema_version: 6 };
    expect(() => parseDashboard(raw)).toThrow();
  });

  it("refuses a grid that is not 8x6", () => {
    const raw = { ...starterDashboard("x"), grid: { cols: 10, rows: 6 } };
    expect(() => parseDashboard(raw)).toThrow();
  });
});

describe("normalizeDashboard", () => {
  it("fills defaults into partial options", () => {
    const normalized = normalizeDashboard(
      doc([mod("a", "calendarNext", 0, 0, 5, 2, {})]),
    );
    expect(normalized.modules[0]?.options).toEqual(
      moduleDefinition("calendarNext").defaultOptions,
    );
  });

  it("keeps a partial text element and completes the rest of it", () => {
    const normalized = normalizeDashboard(
      doc([
        mod("a", "calendarNext", 0, 0, 5, 2, {
          heading: { text: "DEMAIN" },
        }),
      ]),
    );
    const heading = (
      normalized.modules[0]?.options as { heading: TextElement }
    ).heading;
    expect(heading.text).toBe("DEMAIN");
    expect(heading.visible).toBe(true);
    expect(heading.style.size).toBe(15);
    expect(heading.style.family).toBe("inter");
  });

  it("leaves an unknown module type untouched instead of dropping it", () => {
    const normalized = normalizeDashboard(
      doc([mod("a", "teleporter", 0, 0, 2, 1, { warp: 9 })]),
    );
    expect(normalized.modules[0]?.options).toEqual({ warp: 9 });
  });
});

describe("overlaps", () => {
  it("is false for adjacent boxes and true for shared cells", () => {
    const a = mod("a", "haSensor", 0, 0, 2, 2);
    expect(overlaps(a, mod("b", "haSensor", 2, 0, 2, 2))).toBe(false);
    expect(overlaps(a, mod("b", "haSensor", 0, 2, 2, 2))).toBe(false);
    expect(overlaps(a, mod("b", "haSensor", 1, 1, 2, 2))).toBe(true);
    expect(overlaps(a, a)).toBe(true);
  });
});

describe("findFreeSlot", () => {
  it("finds the top-left free position", () => {
    expect(findFreeSlot(emptyDashboard("x"), { w: 2, h: 2 })).toEqual({
      x: 0,
      y: 0,
    });
  });

  it("skips occupied cells", () => {
    const d = doc([mod("a", "weather24h", 0, 0, 8, 3)]);
    expect(findFreeSlot(d, { w: 2, h: 2 })).toEqual({ x: 0, y: 3 });
  });

  it("returns null when the grid is full", () => {
    const d = doc([mod("a", "weather24h", 0, 0, 8, 3)]);
    expect(findFreeSlot(d, { w: 8, h: 6 })).toBeNull();
  });
});

describe("identifiers", () => {
  it("mints distinct ids", () => {
    const ids = new Set(Array.from({ length: 200 }, () => newModuleId()));
    expect(ids.size).toBe(200);
    expect(newDashboardId()).toMatch(/^d_/);
    expect(newModuleId()).toMatch(/^m_/);
  });
});

describe("applyDrag", () => {
  const grid = () =>
    doc([
      mod("weather", "weather24h", 0, 0, 6, 3),
      mod("octo", "octopus", 6, 0, 2, 2),
      mod("sensor", "haSensor", 5, 3, 3, 1),
    ]);

  function moduleOf(d: DashboardDoc, id: string): ModuleInstance {
    const found = d.modules.find((m) => m.id === id);
    if (!found) throw new Error(`no module ${id}`);
    return found;
  }

  it("moves by whole cells", () => {
    const d = grid();
    const moved = applyDrag(d, moduleOf(d, "sensor"), "move", 0, 1);
    expect(moved).toMatchObject({ x: 5, y: 4, w: 3, h: 1 });
  });

  it("clamps a move at the edges of the grid", () => {
    const d = grid();
    expect(applyDrag(d, moduleOf(d, "sensor"), "move", 99, 99)).toMatchObject({
      x: 5,
      y: 5,
    });
    const alone = doc([mod("sensor", "haSensor", 5, 3, 3, 1)]);
    expect(applyDrag(alone, moduleOf(alone, "sensor"), "move", -99, -99)).toMatchObject(
      { x: 0, y: 0 },
    );
  });

  it("refuses a move that would overlap", () => {
    const d = grid();
    // Straight up into the weather block, which occupies rows 0 to 2.
    expect(applyDrag(d, moduleOf(d, "sensor"), "move", -3, -2)).toBeNull();
  });

  it("clamps a resize to the module's declared maximum", () => {
    const alone = doc([mod("sensor", "haSensor", 0, 0, 3, 1)]);
    // haSensor declares a maximum of 4x2.
    expect(applyDrag(alone, moduleOf(alone, "sensor"), "resize", 9, 9)).toMatchObject({
      w: 4,
      h: 2,
    });
  });

  it("clamps a resize to the module's declared minimum", () => {
    const alone = doc([mod("sensor", "haSensor", 0, 0, 3, 1)]);
    expect(applyDrag(alone, moduleOf(alone, "sensor"), "resize", -9, -9)).toMatchObject({
      w: 2,
      h: 1,
    });
  });

  it("stops a resize at the right edge of the grid, ahead of the declared maximum", () => {
    const alone = doc([mod("octo", "octopus", 6, 0, 2, 2)]);
    // The octopus allows 3x3, but three columns from column 6 would run past
    // the eighth. Width stops at 2 while height reaches its own maximum.
    expect(applyDrag(alone, moduleOf(alone, "octo"), "resize", 9, 9)).toMatchObject({
      w: 2,
      h: 3,
    });
  });

  it("stops a resize at the bottom edge of the grid", () => {
    const alone = doc([mod("msg", "message", 0, 5, 6, 1)]);
    // message allows 8x2, but row 5 is the last row.
    expect(applyDrag(alone, moduleOf(alone, "msg"), "resize", 9, 9)).toMatchObject({
      w: 8,
      h: 1,
    });
  });

  it("refuses a resize that would grow into a neighbour", () => {
    const d = grid();
    // Growing the octopus downward would collide with nothing, but growing the
    // weather block would run into the octopus.
    expect(applyDrag(d, moduleOf(d, "weather"), "resize", 2, 0)).toBeNull();
  });

  it("refuses to drag a module of an unknown type", () => {
    const d = doc([mod("x", "teleporter", 0, 0, 2, 1)]);
    expect(applyDrag(d, moduleOf(d, "x"), "move", 1, 0)).toBeNull();
  });
});
