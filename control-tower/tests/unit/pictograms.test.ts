import { describe, expect, it } from "vitest";
import { FrameBuffer, pack } from "@/core/frame";
import { BLACK, RED, WHITE, YELLOW } from "@/core/palette";
import {
  FONT_SIZES,
  measureText,
  resolveRun,
  unsupportedSymbols,
} from "@/core/font";
import { font } from "@/core/render/fonts";
import {
  PICTOGRAMS,
  PICTOGRAM_CATEGORIES,
  PICTOGRAM_GRID,
  TRANSPARENT,
  UNSUPPORTED_PICTOGRAM,
  pictogramAliases,
  pictogramFor,
  pictogramMask,
  pictogramProblems,
  pictogramScale,
  supportedPictograms,
} from "@/core/render/pictograms";
import { moduleDefinition } from "@/core/render/modules";
import { cellsToPixels, type ModuleData } from "@/core/render/types";
import { sha256HexSync } from "@/server/hash";
import { FIXTURE_CTX } from "./fixtures/render";

const atlas = (size = 15) => font("inter", "regular", size);

function inkCount(fb: FrameBuffer, colour: number): number {
  let count = 0;
  for (const value of fb.pixels) if (value === colour) count += 1;
  return count;
}

function drawn(text: string, size = 15): FrameBuffer {
  const fb = new FrameBuffer(WHITE);
  fb.drawText(atlas(size), 4, 4, text, BLACK);
  return fb;
}

describe("the pictogram set", () => {
  it("is well formed: right size, real pigments, nothing blank", () => {
    expect(pictogramProblems()).toEqual([]);
  });

  it("covers every category the household actually writes about", () => {
    const groups = supportedPictograms();
    expect(groups.map((group) => group.category)).toEqual([
      ...PICTOGRAM_CATEGORIES,
    ]);
    for (const group of groups) {
      expect(group.entries.length).toBeGreaterThan(0);
    }
  });

  it("uses only the four physical pigments, at every scale", () => {
    const allowed = new Set<number>([BLACK, WHITE, YELLOW, RED, TRANSPARENT]);
    for (const picto of [...Object.values(PICTOGRAMS), UNSUPPORTED_PICTOGRAM]) {
      for (const scale of [1, 2]) {
        for (const value of pictogramMask(picto, scale).data) {
          expect(allowed.has(value)).toBe(true);
        }
      }
    }
  });

  it("scales by whole pixels only, and only once", () => {
    expect(pictogramScale(11)).toBe(1);
    expect(pictogramScale(18)).toBe(1);
    expect(pictogramScale(22)).toBe(2);
    expect(pictogramScale(34)).toBe(2);
    const mask = pictogramMask(UNSUPPORTED_PICTOGRAM, 2);
    expect(mask.w).toBe(PICTOGRAM_GRID * 2);
    expect(mask.h).toBe(PICTOGRAM_GRID * 2);
  });

  it("points every alias at a pictogram that exists", () => {
    for (const { to } of pictogramAliases()) {
      expect(PICTOGRAMS[to]).toBeDefined();
    }
  });
});

describe("emoji in a message", () => {
  it("is drawn, not dropped", () => {
    const withEmoji = drawn("Parcel 🥚");
    const without = drawn("Parcel");
    expect(sha256HexSync(pack(withEmoji))).not.toBe(
      sha256HexSync(pack(without)),
    );
    expect(measureText(atlas(), "Parcel 🥚")).toBeGreaterThan(
      measureText(atlas(), "Parcel "),
    );
  });

  it("is never a tofu box or a question mark when it is supported", () => {
    expect(unsupportedSymbols(atlas(), "☀️ 🐔 ❤️ ⚠️ 🎉 🍞 🏠 🚗 ⏰ ✅")).toEqual([]);
  });

  it("ignores the variation selector, so both forms draw the same thing", () => {
    expect(sha256HexSync(pack(drawn("☀️")))).toBe(
      sha256HexSync(pack(drawn("☀"))),
    );
  });

  it("ignores a skin tone modifier rather than drawing a second symbol", () => {
    // Not a supported pictogram either way, but the modifier must not become a
    // second box on its own.
    expect(unsupportedSymbols(atlas(), "👍🏽")).toEqual(["👍"]);
  });

  it("brings its own colours: a red heart is red however the text is set", () => {
    const fb = drawn("❤️");
    expect(inkCount(fb, RED)).toBeGreaterThan(20);
  });

  it("keeps the sun yellow and the warning triangle yellow", () => {
    expect(inkCount(drawn("☀️"), YELLOW)).toBeGreaterThan(20);
    expect(inkCount(drawn("⚠️"), YELLOW)).toBeGreaterThan(10);
  });

  it("gets bigger with the type, in one deliberate step", () => {
    const small = measureText(atlas(15), "🥚");
    const large = measureText(atlas(27), "🥚");
    expect(large).toBe(small * 2 - 1);
  });

  it("measures and draws through one run, so wrapping agrees with drawing", () => {
    const text = "Collect the parcel 🥚 before noon ⏰";
    const run = resolveRun(atlas(), text);
    const pictos = run.items.filter((item) => item.kind === "picto");
    expect(pictos).toHaveLength(2);
    expect(measureText(atlas(), text)).toBe(
      run.items.reduce((sum, item) => sum + item.advance, 0),
    );
  });
});

describe("an emoji with no pictogram", () => {
  it("is preserved visibly, as a boxed question mark", () => {
    const fb = drawn("🦄");
    expect(inkCount(fb, BLACK)).toBeGreaterThan(20);
    expect(inkCount(fb, RED)).toBeGreaterThan(5);
    // And it is a different picture from anything supported.
    expect(sha256HexSync(pack(fb))).not.toBe(sha256HexSync(pack(drawn("🐔"))));
  });

  it("is named, so the designer can decide what to do about it", () => {
    expect(unsupportedSymbols(atlas(), "🦄 et 🦓")).toEqual(["🦄", "🦓"]);
  });

  it("is not confused with an unmapped letter, which stays a question mark", () => {
    // A CJK character is not a symbol this panel has a drawing for, but it is
    // also not an emoji: the old question-mark fallback is right for it.
    const run = resolveRun(atlas(), "漢");
    expect(run.items.every((item) => item.kind === "glyph")).toBe(true);
    expect(run.unsupported).toEqual([]);
  });

  it("reports itself to the designer through the module reporter", () => {
    const notes: Array<{ kind: string; symbols?: string[] }> = [];
    const definition = moduleDefinition("message");
    const fb = new FrameBuffer(WHITE);
    definition.render(
      fb,
      cellsToPixels(0, 0, 6, 2),
      { state: "ok" } as ModuleData<never>,
      definition.schema.parse({ body: { text: "Fête 🦄 demain" } }),
      {
        ...FIXTURE_CTX,
        report: {
          overflow: () => undefined,
          note: (fact) => notes.push(fact),
        },
      },
    );
    const note = notes.find((entry) => entry.kind === "unsupported-symbols");
    expect(note?.symbols).toEqual(["🦄"]);
  });
});

describe("pictograms and the palette contract", () => {
  it("packs to exactly 30000 bytes at every size on the ladder", () => {
    for (const size of FONT_SIZES) {
      const fb = new FrameBuffer(WHITE);
      fb.drawText(font("inter", "regular", size), 2, 2, "☀️❤️✅⚠️🎉🥚🐔🏠🚗⏰🦄", BLACK);
      expect(pack(fb)).toHaveLength(30000);
    }
  });

  it("is deterministic: the same string gives the same bytes", () => {
    expect(sha256HexSync(pack(drawn("Household list")))).toBe(
      sha256HexSync(pack(drawn("Household list"))),
    );
  });

  it("resolves the shared list's own emoji", () => {
    expect(pictogramFor("🐥")?.name).toBe("Chick");
    expect(pictogramFor("🍁")?.name).toBe("Maple leaf");
    expect(unsupportedSymbols(atlas(), "Household list")).toEqual([]);
  });

  it("never draws above the line it is on", () => {
    const fb = new FrameBuffer(WHITE);
    const face = font("inter", "regular", 11);
    fb.drawText(face, 4, 20, "🐔", BLACK);
    for (let x = 0; x < 400; x += 1) {
      for (let y = 0; y < 20; y += 1) {
        expect(fb.get(x, y)).toBe(WHITE);
      }
    }
  });
});
