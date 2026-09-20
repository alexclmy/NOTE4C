import { describe, expect, it } from "vitest";
import { FrameBuffer, pack } from "@/core/frame";
import { BLACK, RED, WHITE, YELLOW } from "@/core/palette";
import {
  DEFAULT_THEME,
  DashboardThemeSchema,
  MAX_CONTENT_PADDING,
  PalettePolicySchema,
  applyFamilyToAllStyles,
  applyPalettePolicy,
  colourFor,
  contrastVerdict,
  countStyleInheritance,
  fallbackExplanation,
  pigmentFallback,
  resolveFamily,
  resolveInheritedStyles,
  resolveWeight,
} from "@/core/theme";
import {
  GRID_COLS,
  GRID_ROWS,
  cellsToPixels,
  contentRect,
} from "@/core/render/types";
import {
  DASHBOARD_SCHEMA_VERSION,
  parseDashboard,
  starterDashboard,
} from "@/core/model";
import { migrateDashboardDoc } from "@/core/migrate";
import { renderDashboard, renderDashboardWithReport } from "@/core/render";
import { moduleDefinition } from "@/core/render/modules";
import { sha256HexSync } from "@/server/hash";
import { FIXTURE_CTX, fixtureSources } from "./fixtures/render";

function digest(fb: FrameBuffer): string {
  return sha256HexSync(pack(fb));
}

function fixedStarter() {
  const doc = starterDashboard("Kitchen panel");
  doc.id = "d_fixed";
  doc.createdAt = "2026-09-11T00:00:00.000Z";
  doc.updatedAt = "2026-09-11T00:00:00.000Z";
  doc.modules.forEach((module, index) => {
    module.id = `m${index}`;
  });
  return doc;
}

describe("the default theme changes nothing", () => {
  it("is what a dashboard with no theme gets", () => {
    expect(DEFAULT_THEME).toEqual({
      contentPadding: 0,
      typography: { family: "inter", weight: "regular" },
      palette: { black: true, white: true, red: true, yellow: true },
      // Expression was added in schema 5. Every one of its defaults is the
      // inert value, so a dashboard that gets the default theme still renders
      // exactly what it did before Expression existed.
      expression: {
        colourUse: "balanced",
        brush: "grain",
        pixelTexture: "medium",
        largerText: false,
      },
    });
  });

  it("lays the grid out exactly where the old fixed 50 px cells were", () => {
    for (let x = 0; x < GRID_COLS; x += 1) {
      for (let y = 0; y < GRID_ROWS; y += 1) {
        expect(cellsToPixels(x, y, 1, 1, 0)).toEqual({
          x: x * 50,
          y: y * 50,
          w: 50,
          h: 50,
        });
      }
    }
  });

  it("renders the starter dashboard to the same bytes with and without it", () => {
    const withTheme = renderDashboard(fixedStarter(), fixtureSources(), FIXTURE_CTX);
    // A document that predates themes, as it would arrive off disk before the
    // migration has run. The renderer must not need it to have one.
    const stripped = fixedStarter();
    delete (stripped as Partial<typeof stripped>).theme;
    const without = renderDashboard(stripped, fixtureSources(), FIXTURE_CTX);
    expect(digest(without)).toBe(digest(withTheme));
  });
});

describe("migration to schema 3", () => {
  it("gives a version 2 document the default theme and nothing else", () => {
    const v2 = { ...fixedStarter(), schema_version: 2 } as Record<string, unknown>;
    delete v2.theme;
    const migrated = migrateDashboardDoc(v2) as {
      schema_version: number;
      theme: unknown;
      modules: unknown[];
    };
    expect(migrated.schema_version).toBe(DASHBOARD_SCHEMA_VERSION);
    expect(migrated.theme).toEqual(DEFAULT_THEME);
    expect(migrated.modules).toHaveLength(fixedStarter().modules.length);
  });

  it("paints a migrated version 2 dashboard identically", () => {
    const before = renderDashboard(fixedStarter(), fixtureSources(), FIXTURE_CTX);
    const v2 = { ...fixedStarter(), schema_version: 2 } as Record<string, unknown>;
    delete v2.theme;
    const after = renderDashboard(
      parseDashboard(v2),
      fixtureSources(),
      FIXTURE_CTX,
    );
    expect(digest(after)).toBe(digest(before));
  });

  it("keeps a theme that is already there", () => {
    const v2 = {
      ...fixedStarter(),
      schema_version: 2,
      theme: { ...DEFAULT_THEME, contentPadding: 8 },
    };
    const migrated = migrateDashboardDoc(v2) as { theme: { contentPadding: number } };
    expect(migrated.theme.contentPadding).toBe(8);
  });
});

describe("content padding", () => {
  it("shrinks the content area by the padding on every side", () => {
    expect(contentRect(0)).toEqual({ x: 0, y: 0, w: 400, h: 300 });
    expect(contentRect(10)).toEqual({ x: 10, y: 10, w: 380, h: 280 });
  });

  it("still tiles the whole content area with eight columns and six rows", () => {
    for (const padding of [0, 1, 7, 12, MAX_CONTENT_PADDING]) {
      const content = contentRect(padding);
      let x = content.x;
      for (let column = 0; column < GRID_COLS; column += 1) {
        const cell = cellsToPixels(column, 0, 1, 1, padding);
        // No gap and no overlap: each column starts where the last one ended.
        expect(cell.x).toBe(x);
        x += cell.w;
      }
      expect(x).toBe(content.x + content.w);

      let y = content.y;
      for (let row = 0; row < GRID_ROWS; row += 1) {
        const cell = cellsToPixels(0, row, 1, 1, padding);
        expect(cell.y).toBe(y);
        y += cell.h;
      }
      expect(y).toBe(content.y + content.h);
    }
  });

  it("never lets a module reach the edge of the frame", () => {
    const padding = 12;
    const last = cellsToPixels(GRID_COLS - 1, GRID_ROWS - 1, 1, 1, padding);
    expect(last.x + last.w).toBe(400 - padding);
    expect(last.y + last.h).toBe(300 - padding);
  });

  it("rejects impossible values with a reason", () => {
    const tooMuch = DashboardThemeSchema.safeParse({ contentPadding: 60 });
    expect(tooMuch.success).toBe(false);
    if (!tooMuch.success) {
      expect(tooMuch.error.issues[0]?.message).toMatch(/stops at 24 px/);
    }
    expect(DashboardThemeSchema.safeParse({ contentPadding: -1 }).success).toBe(false);
    expect(DashboardThemeSchema.safeParse({ contentPadding: 4.5 }).success).toBe(false);
    expect(DashboardThemeSchema.safeParse({ contentPadding: 24 }).success).toBe(true);
  });

  it("moves the pixels, and is still a legal frame", () => {
    const padded = fixedStarter();
    padded.theme = { ...DEFAULT_THEME, contentPadding: 16 };
    const before = renderDashboard(fixedStarter(), fixtureSources(), FIXTURE_CTX);
    const after = renderDashboard(padded, fixtureSources(), FIXTURE_CTX);
    expect(digest(after)).not.toBe(digest(before));
    expect(pack(after)).toHaveLength(30000);
    // The margin itself is paper, all the way round.
    for (let x = 0; x < 400; x += 1) {
      expect(after.get(x, 0)).toBe(WHITE);
      expect(after.get(x, 299)).toBe(WHITE);
    }
  });

  it("surfaces the overflow padding causes instead of cropping", () => {
    const doc = fixedStarter();
    doc.theme = { ...DEFAULT_THEME, contentPadding: MAX_CONTENT_PADDING };
    const roomy = renderDashboardWithReport(
      fixedStarter(),
      fixtureSources(),
      FIXTURE_CTX,
    );
    const tight = renderDashboardWithReport(doc, fixtureSources(), FIXTURE_CTX);
    expect(tight.report.overflows.length).toBeGreaterThan(
      roomy.report.overflows.length,
    );
  });
});

describe("palette policy", () => {
  it("refuses to switch off black or white, and says why", () => {
    const noBlack = PalettePolicySchema.safeParse({ black: false });
    expect(noBlack.success).toBe(false);
    if (!noBlack.success) {
      expect(noBlack.error.issues[0]?.message).toMatch(/Black is structural/);
    }
    const noWhite = PalettePolicySchema.safeParse({ white: false });
    expect(noWhite.success).toBe(false);
    if (!noWhite.success) {
      expect(noWhite.error.issues[0]?.message).toMatch(/White is structural/);
    }
  });

  it("maps a disabled accent to an enabled one, deterministically", () => {
    const noRed = PalettePolicySchema.parse({ red: false });
    expect(pigmentFallback(noRed, RED)).toBe(BLACK);
    expect(pigmentFallback(noRed, YELLOW)).toBe(YELLOW);

    const noYellow = PalettePolicySchema.parse({ yellow: false });
    expect(pigmentFallback(noYellow, YELLOW)).toBe(RED);

    const neither = PalettePolicySchema.parse({ red: false, yellow: false });
    expect(pigmentFallback(neither, YELLOW)).toBe(BLACK);
    expect(pigmentFallback(neither, RED)).toBe(BLACK);
    // Black and white never move.
    expect(pigmentFallback(neither, BLACK)).toBe(BLACK);
    expect(pigmentFallback(neither, WHITE)).toBe(WHITE);
  });

  it("explains itself without claiming the pigment is gone", () => {
    const lines = fallbackExplanation(
      PalettePolicySchema.parse({ red: false, yellow: true }),
    );
    expect(lines.join(" ")).toMatch(/still physically there/);
  });

  it("leaves a frame untouched when every pigment is enabled", () => {
    const pixels = Uint8Array.from([BLACK, WHITE, YELLOW, RED]);
    expect(applyPalettePolicy(pixels, DEFAULT_THEME.palette)).toBe(0);
    expect([...pixels]).toEqual([BLACK, WHITE, YELLOW, RED]);
  });

  it("removes every trace of a disabled pigment from the packed bytes", () => {
    const doc = fixedStarter();
    doc.theme = {
      ...DEFAULT_THEME,
      palette: PalettePolicySchema.parse({ red: false, yellow: false }),
    };
    const { frame, report } = renderDashboardWithReport(
      doc,
      fixtureSources(),
      FIXTURE_CTX,
    );
    expect(report.remappedPixels).toBeGreaterThan(0);
    for (const value of frame.pixels) {
      expect(value === BLACK || value === WHITE).toBe(true);
    }
    expect(pack(frame)).toHaveLength(30000);
  });

  it("cannot produce a blank frame, because black stays enabled", () => {
    const doc = fixedStarter();
    doc.theme = {
      ...DEFAULT_THEME,
      palette: PalettePolicySchema.parse({ red: false, yellow: false }),
    };
    const frame = renderDashboard(doc, fixtureSources(), FIXTURE_CTX);
    const ink = [...frame.pixels].filter((value) => value === BLACK).length;
    expect(ink).toBeGreaterThan(500);
  });

  it("is a pure function of the document: same policy, same bytes", () => {
    const build = () => {
      const doc = fixedStarter();
      doc.theme = {
        ...DEFAULT_THEME,
        palette: PalettePolicySchema.parse({ yellow: false }),
      };
      return renderDashboard(doc, fixtureSources(), FIXTURE_CTX);
    };
    expect(digest(build())).toBe(digest(build()));
  });
});

describe("colour tokens", () => {
  it("inherit means whatever the module chose", () => {
    expect(colourFor("inherit", RED)).toBe(RED);
    expect(colourFor("inherit", BLACK)).toBe(BLACK);
  });

  it("names pigments, and only the four that exist", () => {
    expect(colourFor("ink", RED)).toBe(BLACK);
    expect(colourFor("paper", BLACK)).toBe(WHITE);
    expect(colourFor("accent", BLACK)).toBe(RED);
    expect(colourFor("highlight", BLACK)).toBe(YELLOW);
  });

  it("calls out a combination that cannot be read", () => {
    expect(contrastVerdict(WHITE, WHITE)).toBe("invisible");
    expect(contrastVerdict(YELLOW, WHITE)).toBe("low");
    expect(contrastVerdict(BLACK, WHITE)).toBeNull();
    expect(contrastVerdict(RED, WHITE)).toBeNull();
  });

  it("reports an unreadable text role rather than silently correcting it", () => {
    const doc = fixedStarter();
    const message = doc.modules.find((module) => module.type === "message");
    if (!message) throw new Error("the starter has no message");
    message.options = moduleDefinition("message").schema.parse({
      body: { text: "Invisible", style: { colour: "paper" } },
    }) as Record<string, unknown>;

    const { report } = renderDashboardWithReport(
      doc,
      fixtureSources(),
      FIXTURE_CTX,
    );
    const contrast = report.contrasts.find((fact) => fact.role === "body");
    expect(contrast?.verdict).toBe("invisible");
    expect(contrast?.foreground).toBe("white");
    expect(contrast?.background).toBe("white");
  });

  it("draws a text role in the pigment its token names", () => {
    const doc = fixedStarter();
    const message = doc.modules.find((module) => module.type === "message");
    if (!message) throw new Error("the starter has no message");
    const withToken = (colour: string) => {
      message.options = moduleDefinition("message").schema.parse({
        body: { text: "Parcel", style: { colour } },
      }) as Record<string, unknown>;
      return renderDashboard(doc, fixtureSources(), FIXTURE_CTX);
    };
    const inherited = withToken("inherit");
    const accented = withToken("accent");
    expect(digest(accented)).not.toBe(digest(inherited));
    // The module's own choice for the body is ink, so accent means red ink.
    const redPixels = [...accented.pixels].filter((v) => v === RED).length;
    const redBefore = [...inherited.pixels].filter((v) => v === RED).length;
    expect(redPixels).toBeGreaterThan(redBefore);
  });
});

describe("font inheritance", () => {
  it("resolves inherit against the dashboard, and leaves overrides alone", () => {
    const theme = { ...DEFAULT_THEME, typography: { family: "poppins" as const, weight: "bold" as const } };
    expect(resolveFamily("inherit", theme)).toBe("poppins");
    expect(resolveFamily("plexmono", theme)).toBe("plexmono");
    expect(resolveWeight("inherit", theme)).toBe("bold");
    expect(resolveWeight("regular", theme)).toBe("regular");
  });

  it("rewrites every style in an options tree, and nothing else", () => {
    const theme = { ...DEFAULT_THEME, typography: { family: "atkinson" as const, weight: "regular" as const } };
    const options = {
      body: { text: "Parcel", visible: true, style: { family: "inherit", size: 15, weight: "bold" } },
      rows: [{ text: "Un", visible: true }],
      maxRows: 4,
    };
    const resolved = resolveInheritedStyles(options, theme) as typeof options;
    expect(resolved.body.style.family).toBe("atkinson");
    // An explicit weight is an override and survives.
    expect(resolved.body.style.weight).toBe("bold");
    expect(resolved.body.text).toBe("Parcel");
    expect(resolved.rows).toEqual(options.rows);
    expect(resolved.maxRows).toBe(4);
  });

  it("counts what inherits and what does not", () => {
    const options = moduleDefinition("weather24h").defaultOptions;
    const counted = countStyleInheritance(options);
    expect(counted.total).toBeGreaterThan(3);
    // Nothing inherits until somebody asks for it: defaults are overrides, so
    // a new dashboard font changes nothing until it is applied.
    expect(counted.inheriting).toBe(0);
    expect(counted.overridden).toBe(counted.total);
  });

  it("apply-to-all makes every role inherit, and only then", () => {
    const options = moduleDefinition("weather24h").defaultOptions;
    const applied = applyFamilyToAllStyles(options);
    const counted = countStyleInheritance(applied);
    expect(counted.inheriting).toBe(counted.total);
    expect(counted.total).toBe(countStyleInheritance(options).total);
  });

  it("changes the rendered pixels only once a role inherits", () => {
    const base = fixedStarter();
    const themed = fixedStarter();
    themed.theme = {
      ...DEFAULT_THEME,
      typography: { family: "poppins", weight: "regular" },
    };
    // Nobody inherits yet.
    expect(digest(renderDashboard(themed, fixtureSources(), FIXTURE_CTX))).toBe(
      digest(renderDashboard(base, fixtureSources(), FIXTURE_CTX)),
    );

    themed.modules = themed.modules.map((module) => ({
      ...module,
      options: applyFamilyToAllStyles(module.options),
    }));
    expect(
      digest(renderDashboard(themed, fixtureSources(), FIXTURE_CTX)),
    ).not.toBe(digest(renderDashboard(base, fixtureSources(), FIXTURE_CTX)));
  });

  it("survives a round trip through the document schema", () => {
    const doc = fixedStarter();
    doc.theme = {
      contentPadding: 6,
      typography: { family: "poppins", weight: "bold" },
      palette: { black: true, white: true, red: true, yellow: false },
      expression: {
        colourUse: "expressive",
        brush: "halftone",
        pixelTexture: "large",
        largerText: true,
      },
    };
    doc.modules = doc.modules.map((module) => ({
      ...module,
      options: applyFamilyToAllStyles(module.options),
    }));
    const round = parseDashboard(JSON.parse(JSON.stringify(doc)));
    expect(round.theme).toEqual(doc.theme);
    expect(countStyleInheritance(round.modules[0]?.options).inheriting).toBe(
      countStyleInheritance(round.modules[0]?.options).total,
    );
  });
});
