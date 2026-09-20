import { describe, expect, it } from "vitest";
import { FrameBuffer, pack } from "@/core/frame";
import {
  DASHBOARD_SCHEMA_VERSION,
  parseDashboard,
  starterDashboard,
  type DashboardDoc,
} from "@/core/model";
import { migrateDashboardDoc, migrateDashboardDocV4toV5 } from "@/core/migrate";
import {
  DEFAULT_EXPRESSION,
  DashboardThemeSchema,
  applyLargerText,
} from "@/core/theme";
import { renderDashboard } from "@/core/render";
import { sha256HexSync } from "@/server/hash";
import { FIXTURE_CTX, fixtureSources } from "./fixtures/render";

const digest = (fb: FrameBuffer): string => sha256HexSync(pack(fb));

describe("the Expression migration", () => {
  it("is the current schema version", () => {
    expect(DASHBOARD_SCHEMA_VERSION).toBe(5);
  });

  it("lifts a v4 document to v5 by giving its theme the default expression", () => {
    const v4 = {
      schema_version: 4,
      id: "d1",
      title: "Legacy",
      status: "active",
      grid: { cols: 8, rows: 6 },
      theme: {
        contentPadding: 0,
        typography: { family: "inter", weight: "regular" },
        palette: { black: true, white: true, red: true, yellow: true },
      },
      refreshIntervalMinutes: null,
      modules: [],
      createdAt: "2026-01-01T00:00:00.000Z",
      updatedAt: "2026-01-01T00:00:00.000Z",
    };
    const migrated = migrateDashboardDocV4toV5(v4) as {
      schema_version: number;
      theme: { expression: unknown };
    };
    expect(migrated.schema_version).toBe(5);
    expect(migrated.theme.expression).toEqual(DEFAULT_EXPRESSION);
    // And it parses through the document schema without complaint.
    expect(() => parseDashboard(v4)).not.toThrow();
  });

  it("keeps an expression a document already carries", () => {
    const withExpr = {
      schema_version: 4,
      theme: { expression: { colourUse: "expressive" } },
    };
    const migrated = migrateDashboardDocV4toV5(withExpr) as {
      theme: { expression: { colourUse: string } };
    };
    expect(migrated.theme.expression.colourUse).toBe("expressive");
  });

  it("changes nothing on a dashboard that does not use the dither engine", () => {
    // The default expression must be inert. A starter dashboard, which has no
    // Headline or Sky, renders identically whether the expression is spelled
    // out or left to default.
    const base = fixedStarter();
    const explicit: DashboardDoc = {
      ...base,
      theme: DashboardThemeSchema.parse({
        ...base.theme,
        expression: DEFAULT_EXPRESSION,
      }),
    };
    expect(digest(renderDashboard(base, fixtureSources(), FIXTURE_CTX))).toBe(
      digest(renderDashboard(explicit, fixtureSources(), FIXTURE_CTX)),
    );
  });
});

describe("larger text", () => {
  it("steps a role's size up the ladder and leaves it alone when off", () => {
    const style = { family: "inter", size: 15, weight: "regular" };
    expect(applyLargerText({ style }, false)).toEqual({ style });
    const bigger = applyLargerText({ style }, true) as { style: { size: number } };
    expect(bigger.style.size).toBe(18);
  });

  it("changes the rendered frame when switched on", () => {
    const base = fixedStarter();
    const larger: DashboardDoc = {
      ...base,
      theme: DashboardThemeSchema.parse({
        ...base.theme,
        expression: { ...DEFAULT_EXPRESSION, largerText: true },
      }),
    };
    expect(digest(renderDashboard(base, fixtureSources(), FIXTURE_CTX))).not.toBe(
      digest(renderDashboard(larger, fixtureSources(), FIXTURE_CTX)),
    );
  });
});

describe("the full migration chain", () => {
  it("carries a v1 document all the way to v5", () => {
    const v1 = {
      schema_version: 1,
      id: "d",
      title: "Old",
      status: "active",
      grid: { cols: 8, rows: 6 },
      modules: [],
      createdAt: "2026-01-01T00:00:00.000Z",
      updatedAt: "2026-01-01T00:00:00.000Z",
    };
    const out = migrateDashboardDoc(v1) as { schema_version: number };
    expect(out.schema_version).toBe(5);
    expect(() => parseDashboard(v1)).not.toThrow();
  });
});

/** A starter with a fixed id/time so digests do not drift. */
function fixedStarter(): DashboardDoc {
  const doc = starterDashboard("Fixture", new Date("2026-01-01T00:00:00.000Z"));
  return { ...doc, id: "fixture", createdAt: "2026-01-01T00:00:00.000Z", updatedAt: "2026-01-01T00:00:00.000Z" };
}
