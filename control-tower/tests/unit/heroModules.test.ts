import { describe, expect, it } from "vitest";
import { FrameBuffer, pack } from "@/core/frame";
import { WHITE } from "@/core/palette";
import { moduleDefinition } from "@/core/render/modules";
import { HEADLINE_VARIANTS } from "@/core/render/modules/headline";
import { SKY_VARIANTS } from "@/core/render/modules/sky";
import { cellsToPixels, type ModuleData, type RenderContext } from "@/core/render/types";
import { countIsolatedAccents } from "@/core/render/dither";
import { DashboardThemeSchema, type Expression } from "@/core/theme";
import { sha256HexSync } from "@/server/hash";

const NOW = new Date("2026-09-19T09:20:00.000Z");

function ctxFor(expression: Partial<Expression>): RenderContext {
  const theme = DashboardThemeSchema.parse({ expression });
  return { now: NOW, timeZone: "Europe/Paris", theme };
}

function renderModule(
  type: string,
  span: { w: number; h: number },
  options: Record<string, unknown>,
  ctx: RenderContext,
): FrameBuffer {
  const definition = moduleDefinition(type);
  const fb = new FrameBuffer(WHITE);
  definition.render(
    fb,
    cellsToPixels(0, 0, span.w, span.h),
    { state: "ok" } as ModuleData<unknown>,
    definition.schema.parse(options),
    ctx,
  );
  return fb;
}

const digest = (fb: FrameBuffer): string => sha256HexSync(pack(fb));

describe("headline module", () => {
  it("packs cleanly and leaves no isolated accent, every variant and stance", () => {
    for (const variant of HEADLINE_VARIANTS) {
      for (const colourUse of ["blackwhite", "balanced", "expressive"] as const) {
        const fb = renderModule(
          "headline",
          { w: 8, h: 2 },
          { variant },
          ctxFor({ colourUse }),
        );
        expect(() => pack(fb)).not.toThrow();
        expect(countIsolatedAccents(fb)).toBe(0);
      }
    }
  });

  it("is deterministic: same options and context, byte-identical", () => {
    const once = renderModule("headline", { w: 8, h: 2 }, {}, ctxFor({}));
    const twice = renderModule("headline", { w: 8, h: 2 }, {}, ctxFor({}));
    expect(digest(once)).toBe(digest(twice));
  });

  it("spends no red or yellow in black & white, and some in expressive", () => {
    const count = (fb: FrameBuffer, colour: number): number => {
      let n = 0;
      for (const p of fb.pixels) if (p === colour) n += 1;
      return n;
    };
    const bw = renderModule(
      "headline",
      { w: 8, h: 2 },
      { variant: "sidebar" },
      ctxFor({ colourUse: "blackwhite" }),
    );
    const expressive = renderModule(
      "headline",
      { w: 8, h: 2 },
      { variant: "sidebar" },
      ctxFor({ colourUse: "expressive" }),
    );
    expect(count(bw, 3) + count(bw, 2)).toBe(0); // no red, no yellow
    expect(count(expressive, 3)).toBeGreaterThan(0); // warm ink is spent
  });

  it("changes the frame when the brush changes", () => {
    const grain = renderModule("headline", { w: 8, h: 2 }, {}, ctxFor({ brush: "grain" }));
    const grid = renderModule("headline", { w: 8, h: 2 }, {}, ctxFor({ brush: "grid" }));
    expect(digest(grain)).not.toBe(digest(grid));
  });
});

describe("sky module", () => {
  it("packs cleanly and leaves no isolated accent, every variant and stance", () => {
    for (const variant of SKY_VARIANTS) {
      for (const colourUse of ["blackwhite", "balanced", "expressive"] as const) {
        const fb = renderModule(
          "sky",
          { w: 5, h: 3 },
          { variant, latitude: 48.86, longitude: 2.35 },
          ctxFor({ colourUse }),
        );
        expect(() => pack(fb)).not.toThrow();
        expect(countIsolatedAccents(fb)).toBe(0);
      }
    }
  });

  it("survives the polar cases without throwing", () => {
    const midnightSun = renderModule(
      "sky",
      { w: 5, h: 3 },
      { latitude: 78.2, longitude: 15.6 },
      { ...ctxFor({}), now: new Date("2026-06-21T12:00:00Z") },
    );
    expect(() => pack(midnightSun)).not.toThrow();
    const polarNight = renderModule(
      "sky",
      { w: 5, h: 3 },
      { latitude: 78.2, longitude: 15.6 },
      { ...ctxFor({}), now: new Date("2026-12-21T12:00:00Z") },
    );
    expect(() => pack(polarNight)).not.toThrow();
  });

  it("is deterministic for a fixed clock and location", () => {
    const a = renderModule("sky", { w: 5, h: 3 }, { latitude: 48.86, longitude: 2.35 }, ctxFor({}));
    const b = renderModule("sky", { w: 5, h: 3 }, { latitude: 48.86, longitude: 2.35 }, ctxFor({}));
    expect(digest(a)).toBe(digest(b));
  });
});
