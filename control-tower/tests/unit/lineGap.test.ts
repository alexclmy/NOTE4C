import { describe, expect, it } from "vitest";
import { FrameBuffer, pack } from "@/core/frame";
import { WHITE } from "@/core/palette";
import { FONT_FAMILY_IDS, FONT_SIZES } from "@/core/font";
import { moduleDefinition } from "@/core/render/modules";
import { cellsToPixels, type ModuleData } from "@/core/render/types";
import { atlasFor, layoutText, lineStep, type LayoutNote } from "@/core/render/text";
import { sha256HexSync } from "@/server/hash";
import { FIXTURE_CTX } from "./fixtures/render";

/**
 * The reported bug was "changing Line gap on a Message does nothing".
 *
 * It is real, and its cause is not the lineSpacing pipeline. In a tile only
 * tall enough for one line, the distance BETWEEN lines has nothing to move.
 * The starter's Message is three cells by one, which is 42 px of content after
 * padding, and at 18 px and up two lines need more than that. Anyone
 * reproducing from the existing overflow spec, which drives that tile to
 * 27 px, sees an inert control and blames the control.
 *
 * So these tests pin both halves of the honest fix: the gap really does move
 * pixels wherever a second line can fit, and where it cannot, the renderer
 * says so instead of pretending.
 */

const LONG =
  "Ceci est une note assez longue pour envelopper sur plusieurs lignes du panneau";

function messageFrame(options: {
  lineSpacing: number;
  size?: number;
  cells?: { w: number; h: number };
  family?: (typeof FONT_FAMILY_IDS)[number];
  text?: string;
}): FrameBuffer {
  const definition = moduleDefinition("message");
  const fb = new FrameBuffer(WHITE);
  const cells = options.cells ?? { w: 6, h: 2 };
  definition.render(
    fb,
    cellsToPixels(0, 0, cells.w, cells.h),
    { state: "ok" } as ModuleData<never>,
    definition.schema.parse({
      body: {
        text: options.text ?? LONG,
        style: {
          size: options.size ?? 15,
          lineSpacing: options.lineSpacing,
          ...(options.family ? { family: options.family } : {}),
        },
      },
    }),
    FIXTURE_CTX,
  );
  return fb;
}

function digest(fb: FrameBuffer): string {
  return sha256HexSync(pack(fb));
}

function differingPixels(a: FrameBuffer, b: FrameBuffer): number {
  let count = 0;
  for (let i = 0; i < a.pixels.length; i += 1) {
    if (a.pixels[i] !== b.pixels[i]) count += 1;
  }
  return count;
}

describe("line gap moves the pixels", () => {
  it("two valid gaps lay a multiline message out differently", () => {
    const tight = messageFrame({ lineSpacing: 0 });
    const loose = messageFrame({ lineSpacing: 6 });
    expect(digest(loose)).not.toBe(digest(tight));
    expect(differingPixels(tight, loose)).toBeGreaterThan(100);
  });

  it("every step of the control is a distinct frame", () => {
    const seen = new Map<string, number>();
    for (const gap of [0, 1, 2, 3, 4, 6, 8, 12]) {
      seen.set(digest(messageFrame({ lineSpacing: gap })), gap);
    }
    expect(seen.size).toBe(8);
  });

  it("is the baseline-to-baseline step, exactly", () => {
    const style = {
      family: "inter" as const,
      size: 15 as const,
      weight: "regular" as const,
      align: "left" as const,
      lineSpacing: 0,
      colour: "inherit" as const,
    };
    const atlas = atlasFor(style);
    expect(lineStep(style)).toBe(atlas.lineHeight);
    expect(lineStep({ ...style, lineSpacing: 5 })).toBe(atlas.lineHeight + 5);
  });

  it("holds for every family at a size where two lines fit", () => {
    for (const family of FONT_FAMILY_IDS) {
      const tight = messageFrame({ lineSpacing: 0, family, size: 13 });
      const loose = messageFrame({ lineSpacing: 4, family, size: 13 });
      expect(digest(loose), `${family} ignored the line gap`).not.toBe(
        digest(tight),
      );
    }
  });

  it("changes how many lines fit, which is what a gap is for", () => {
    const element = {
      text: LONG,
      visible: true,
      style: {
        family: "inter" as const,
        size: 15 as const,
        weight: "regular" as const,
        align: "left" as const,
        lineSpacing: 0,
        colour: "inherit" as const,
      },
    };
    const box = { x: 0, y: 0, w: 150, h: 92 };
    const tight = layoutText(element, box, "body", { wrap: true });
    const loose = layoutText(
      { ...element, style: { ...element.style, lineSpacing: 12 } },
      box,
      "body",
      { wrap: true },
    );
    expect(tight?.visibleLines).toBeGreaterThan(loose?.visibleLines ?? 0);
  });
});

describe("a box that only fits one line says so", () => {
  function notesFor(cells: { w: number; h: number }, size: number): LayoutNote[] {
    const notes: LayoutNote[] = [];
    const definition = moduleDefinition("message");
    definition.render(
      new FrameBuffer(WHITE),
      cellsToPixels(0, 0, cells.w, cells.h),
      { state: "ok" } as ModuleData<never>,
      definition.schema.parse({
        body: { text: LONG, style: { size, lineSpacing: 6 } },
      }),
      {
        ...FIXTURE_CTX,
        report: { overflow: () => undefined, note: (fact) => notes.push(fact) },
      },
    );
    return notes.filter((note) => note.kind === "line-gap-inert");
  }

  it("warns on the one-row tile at 27 px, which is the reported case", () => {
    const notes = notesFor({ w: 3, h: 1 }, 27);
    expect(notes).toHaveLength(1);
    expect(notes[0]?.role).toBe("body");
    expect(notes[0]?.detail).toMatch(/fits one line at 27 px/);
  });

  it("stays quiet when a second line does fit", () => {
    expect(notesFor({ w: 3, h: 1 }, 15)).toEqual([]);
    expect(notesFor({ w: 6, h: 2 }, 27)).toEqual([]);
  });

  it("agrees with what the layout actually did, at every size", () => {
    for (const size of FONT_SIZES) {
      const inert = notesFor({ w: 3, h: 1 }, size).length > 0;
      const tight = messageFrame({
        lineSpacing: 0,
        size,
        cells: { w: 3, h: 1 },
      });
      const loose = messageFrame({
        lineSpacing: 8,
        size,
        cells: { w: 3, h: 1 },
      });
      // The note is true exactly when the control cannot change the pixels.
      expect(digest(tight) === digest(loose), `size ${size}`).toBe(inert);
    }
  });

  it("is a note, not an overflow: the text may still fit perfectly", () => {
    const notes: LayoutNote[] = [];
    const overflows: unknown[] = [];
    const definition = moduleDefinition("message");
    definition.render(
      new FrameBuffer(WHITE),
      cellsToPixels(0, 0, 3, 1),
      { state: "ok" } as ModuleData<never>,
      definition.schema.parse({
        body: { text: "Court", style: { size: 27 } },
      }),
      {
        ...FIXTURE_CTX,
        report: {
          overflow: (fact) => overflows.push(fact),
          note: (fact) => notes.push(fact),
        },
      },
    );
    expect(overflows).toEqual([]);
    expect(notes.map((note) => note.kind)).toEqual(["line-gap-inert"]);
  });
});
