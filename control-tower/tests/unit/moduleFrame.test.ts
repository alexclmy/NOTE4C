import { describe, expect, it } from "vitest";
import { FrameBuffer } from "@/core/frame";
import { BLACK, RED, WHITE } from "@/core/palette";
import { drawModuleFrame } from "@/core/render/frame";
import type { PixelRect } from "@/core/render/types";
import {
  DASHBOARD_SCHEMA_VERSION,
  ModuleFrameSchema,
  ModuleInstanceSchema,
  emptyDashboard,
  parseDashboard,
  type DashboardDoc,
  type ModuleInstance,
} from "@/core/model";
import { renderDashboard } from "@/core/render";

const RECT: PixelRect = { x: 100, y: 60, w: 80, h: 50 };

describe("drawModuleFrame", () => {
  it("draws a solid right-edge rule as a black column at the rect's right, grown inward", () => {
    const fb = new FrameBuffer(WHITE);
    drawModuleFrame(fb, RECT, { edges: ["right"], weight: 2 });
    const rightCol = RECT.x + RECT.w - 1; // 179
    // The two inward columns are inked, the column just left of them is not.
    expect(fb.get(rightCol, RECT.y)).toBe(BLACK);
    expect(fb.get(rightCol - 1, RECT.y + RECT.h - 1)).toBe(BLACK);
    expect(fb.get(rightCol - 2, RECT.y + 10)).toBe(WHITE);
  });

  it("draws a bottom-edge rule inside the rect, never below it", () => {
    const fb = new FrameBuffer(WHITE);
    drawModuleFrame(fb, RECT, { edges: ["bottom"], weight: 3 });
    const bottomRow = RECT.y + RECT.h - 1; // 109
    expect(fb.get(RECT.x + 5, bottomRow)).toBe(BLACK);
    expect(fb.get(RECT.x + 5, bottomRow - 2)).toBe(BLACK);
    // Nothing painted on the row that belongs to the neighbour below.
    expect(fb.get(RECT.x + 5, RECT.y + RECT.h)).toBe(WHITE);
    // Nor above the 3px band.
    expect(fb.get(RECT.x + 5, bottomRow - 3)).toBe(WHITE);
  });

  it("inset trims the rule off both ends", () => {
    const fb = new FrameBuffer(WHITE);
    drawModuleFrame(fb, RECT, { edges: ["top"], weight: 1, inset: 0.25 });
    // 25% of 80 = 20px trimmed each end: the corners are clear, the middle inked.
    expect(fb.get(RECT.x, RECT.y)).toBe(WHITE);
    expect(fb.get(RECT.x + RECT.w - 1, RECT.y)).toBe(WHITE);
    expect(fb.get(RECT.x + 40, RECT.y)).toBe(BLACK);
  });

  it("a dashed rule leaves gaps", () => {
    const fb = new FrameBuffer(WHITE);
    drawModuleFrame(fb, RECT, { edges: ["top"], weight: 1, style: "dashed" });
    let inked = 0;
    for (let x = RECT.x; x < RECT.x + RECT.w; x += 1) {
      if (fb.get(x, RECT.y) === BLACK) inked += 1;
    }
    expect(inked).toBeGreaterThan(0);
    expect(inked).toBeLessThan(RECT.w); // not a solid run
  });

  it("honours a non-default rule colour", () => {
    const fb = new FrameBuffer(WHITE);
    drawModuleFrame(fb, RECT, { edges: ["left"], weight: 1, color: RED });
    expect(fb.get(RECT.x, RECT.y + 5)).toBe(RED);
  });

  it("no edges paints nothing", () => {
    const fb = new FrameBuffer(WHITE);
    drawModuleFrame(fb, RECT, { edges: [] });
    expect(fb.pixels.every((p) => p === WHITE)).toBe(true);
  });
});

describe("compositor draws module frames", () => {
  function docWith(frame: ModuleInstance["frame"]): DashboardDoc {
    return {
      ...emptyDashboard("Framed"),
      modules: [
        {
          id: "m1",
          type: "message",
          x: 0,
          y: 0,
          w: 4,
          h: 2,
          hidden: false,
          options: { body: { text: "hi", visible: true } },
          ...(frame ? { frame } : {}),
        },
      ],
    };
  }

  it("renders a right-edge rule at the module seam", () => {
    const framed = renderDashboard(
      parseDashboard(docWith({ edges: ["right"], weight: 2 } as never)),
    );
    const plain = renderDashboard(parseDashboard(docWith(undefined)));
    // A 4-cell-wide module on an 8-col/400px panel ends near x≈199. Somewhere
    // in the module's right edge column, the framed render inked black that the
    // plain one did not.
    let seamInk = 0;
    for (let x = 190; x < 205; x += 1) {
      for (let y = 5; y < 95; y += 1) {
        if (framed.get(x, y) === BLACK && plain.get(x, y) !== BLACK) seamInk += 1;
      }
    }
    expect(seamInk).toBeGreaterThan(50);
  });
});

describe("ModuleFrame schema", () => {
  it("fills defaults from just edges", () => {
    const parsed = ModuleFrameSchema.parse({ edges: ["right"] });
    expect(parsed).toEqual({
      edges: ["right"],
      weight: 2,
      style: "solid",
      inset: 0,
      color: 0,
    });
  });

  it("round-trips on a module through parseDashboard", () => {
    const doc: DashboardDoc = {
      ...emptyDashboard("RT"),
      schema_version: DASHBOARD_SCHEMA_VERSION,
      modules: [
        ModuleInstanceSchema.parse({
          id: "m1",
          type: "message",
          x: 0,
          y: 0,
          w: 4,
          h: 2,
          options: { body: { text: "hi", visible: true } },
          frame: { edges: ["right", "bottom"], weight: 3, style: "dashed" },
        }),
      ],
    };
    const restored = parseDashboard(doc);
    expect(restored.modules[0]?.frame).toEqual({
      edges: ["right", "bottom"],
      weight: 3,
      style: "dashed",
      inset: 0,
      color: 0,
    });
  });

  it("a module with no frame stays frameless after a round-trip", () => {
    const restored = parseDashboard(emptyDashboard("Empty"));
    expect(restored.modules).toEqual([]);
  });
});
