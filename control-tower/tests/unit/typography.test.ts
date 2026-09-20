import { describe, expect, it } from "vitest";
import { FrameBuffer, pack } from "@/core/frame";
import { BLACK, RED, WHITE } from "@/core/palette";
import { sha256HexSync } from "@/server/hash";
import {
  FONT_FAMILY_IDS,
  FONT_SIZES,
  FONT_WEIGHTS,
  measureText,
  wrapText,
} from "@/core/font";
import { font, FONT_FAMILY_META } from "@/core/render/fonts";
import {
  TextStyleSchema,
  drawText,
  fillTemplate,
  layoutText,
  reservedHeight,
  textElementSchema,
  type TextElement,
} from "@/core/render/text";
import { moduleDefinition, MODULE_TYPES } from "@/core/render/modules";
import { migrateDashboardDoc, migrateModuleOptions } from "@/core/migrate";
import {
  DASHBOARD_SCHEMA_VERSION,
  parseDashboard,
  starterDashboard,
} from "@/core/model";
import { renderDashboardWithReport, renderDashboard } from "@/core/render";
import { cellsToPixels, type ModuleData } from "@/core/render/types";
import { FIXTURE_CTX, WEATHER_OK, fixtureSources } from "./fixtures/render";

/**
 * The string every font golden is measured on. It deliberately contains the
 * characters that go wrong first on a small four-colour panel: the I l 1 and
 * O 0 confusions Atkinson Hyperlegible exists to fix, the French accents the
 * panel's copy is written in, the degree sign, and the ellipsis the truncator
 * falls back to.
 */
const SPECIMEN = "Il1O0 gq Ça éèêà ÔÙÏ 19°C … 24h";

/**
 * One digest per family, weight and size: 4 x 2 x 7 = 56 frames of the
 * specimen above, packed. Together they pin the entire glyph pipeline. A
 * change to a vendored face, to the size ladder, or to the rasteriser moves
 * exactly the entries it should, and a change that moves all 56 was a change
 * to the rasteriser whether or not that was the intention.
 */
const FONT_GOLDEN: Record<string, string> = {
  "inter/regular/11":
    "d009dc5e11c13c4c1564587a457394c05dec37695392e07ae690c0fc609ff316",
  "inter/regular/13":
    "81d7303d9b140b311f99b781ef2f767dbe329385e344149975ecd15b2176d945",
  "inter/regular/15":
    "d1070130188d2c4fbcf00fa98adb2c8166b3dc61a19eba1f70582aba0f770598",
  "inter/regular/18":
    "bc719904d4f70fd400cabd6cb297f3b9488b57a6bd2be153ff667e0a3c6dbd4d",
  "inter/regular/22":
    "22ed98a27117d2d0c914f6e59799e6a8d2e588bac64be3f304036fa8bd707b74",
  "inter/regular/27":
    "98126c476eeef03fe7d7b2016cbeb8329536c1df8d29bca9921c1b1775bbd9ce",
  "inter/regular/34":
    "4570f238101c826b076e3b3f984bdfc1fbe027075bf85cb14d178847d5308d75",
  "inter/bold/11":
    "91039db78a33e16fc35ca64ce53f08d7a0d0822a80c9bd8c363823d04f649a57",
  "inter/bold/13":
    "988794688fbeb3496039d0e148e6f5a7c5fc9a63f43044197e29fe52d0d4848d",
  "inter/bold/15":
    "de8d44f44d03d82861ac39a255ad1c1149bb9c863514a2ca4142c99466f86198",
  "inter/bold/18":
    "2a6ced430df2fc6ff60bd7c33154cd89bf6d1a361bad5a504c9258cf6303b5c9",
  "inter/bold/22":
    "9fe7492f43b3a11325e5b55ed1e2d62ba135498b96c1024cbc0cac11389d9785",
  "inter/bold/27":
    "d24f769ed2e7da71a864a91a58f639ab501c5f3ff3ff625e51ef10c8a549c45e",
  "inter/bold/34":
    "d520d73a2408b7ea647722cd5dbb04bf37b4e6971beb7d8a033d2136f74e873e",
  "atkinson/regular/11":
    "44457f1cec20693da8602b5070cab620afb0f7a4a63f1a05aeb3816a98bfadf5",
  "atkinson/regular/13":
    "03113fafd6d5da39a383e42e6605a6750aa0b38ba7b68860af7c7bcd43b43134",
  "atkinson/regular/15":
    "b4ac95175621b39ae6f03adb6d9eb169237e2964a41d845814cb51d538e08526",
  "atkinson/regular/18":
    "27a12856ec26ab2f31dae97a6157ba8e287c2a0addbec1c3fdacdb8b652a378e",
  "atkinson/regular/22":
    "fd435a930d4b113c5140e53eacef6fb54309e54c80806bec2611ee288b9c895b",
  "atkinson/regular/27":
    "f0ef4c3857ddff3d43d4be173f60dd719bdb82aaa114687ed7407d91440be950",
  "atkinson/regular/34":
    "27b3285a64b4e9e66910cc1b748798a6fa7968fc2585ab8f39a1e578eabcdb78",
  "atkinson/bold/11":
    "41ced8393bcee1559862a5c0a113cdb2190e217e7ac88f2d41940d4d03172737",
  "atkinson/bold/13":
    "224039d443e7117b3f355f08eef219a5cdde77d59b9168bb120fc89646643abe",
  "atkinson/bold/15":
    "541ba03b1e1085358fe0f585d5669f5bc9f406ce9fda91ae3fd7b1be37bf69dc",
  "atkinson/bold/18":
    "ff9c7d1a2c463cdb959a77d465a2d1c8c5b89e5e1c289002210a815276d1713e",
  "atkinson/bold/22":
    "1b80c88b65e1bd9e918924053d08b065bd8ebd4b79ebbd7283f68446f0747387",
  "atkinson/bold/27":
    "f37ef1cdf56197a91d628e509003b063549a2faf5890d8a951019c536c1be259",
  "atkinson/bold/34":
    "8e37bf1eae92c827ec9bbf859161fa573b49ccedbf06bfb600af3bd2b25b2dc4",
  "plexmono/regular/11":
    "352a3f8f291e17c06df07a9ce783bd177cdd77fd2121c1d84030a2830c241b37",
  "plexmono/regular/13":
    "2712871177dd54ab92462ce5246dc227820b7d36ec09375bd4744cff6a89e9ed",
  "plexmono/regular/15":
    "4782496839b062993119caeb249cea02acdc6e202220e3fff322e6940ded9e08",
  "plexmono/regular/18":
    "16dbe28a774ce46808d6ba0ce28e26e4f1c57a0bd3d1793d9170fbce66f8e803",
  "plexmono/regular/22":
    "55dc39c58785d6c29f741bd2983ff7d9f17f006fe0ae6e3d421ed1a637235d15",
  "plexmono/regular/27":
    "2406ea7e0db273fad32f7800de9c33ced66d6074faa105f1a89318cf2284738a",
  "plexmono/regular/34":
    "7e912691fcff3369b907a91102afe9a6fd3be483d433d2ddf8910bde1fcfd5d5",
  "plexmono/bold/11":
    "59acfd0b80c9e51c79e8667d0cd304ddeff332dab6c4ca49a53b95c3f4e316c0",
  "plexmono/bold/13":
    "49928dee88059e5d9dca9f988fdeedd388b6506027a9fb9e5cd38f5f32221bd1",
  "plexmono/bold/15":
    "8f424a2aae5fd9c36091a02b04742785a041408623969981de495c311160f772",
  "plexmono/bold/18":
    "35d4dff77d5c2fe257e9fa028f1043daccfbea5d055e9b5cb86e38648b428286",
  "plexmono/bold/22":
    "49c07d66bf15acbdc2800adc73a55a6fb5aa5946f44fb6f6006def05db20c8b7",
  "plexmono/bold/27":
    "0956cbc883f8c928482c6642c215e8dcc4b28f10c22aedb2bc838dc316ec5385",
  "plexmono/bold/34":
    "bddc4975637a998cf84aacc1e468ac80b96632b5120e6b23caf8f8ac382cfaf7",

  "poppins/regular/11":
    "be0635181691045e4045135bd023ac936f0c7cc16ee6d9d393179661252e7088",
  "poppins/regular/13":
    "9f4c228176487145e4619b110299374ffcbbef1c93314247e4a13dc4ccc3fb56",
  "poppins/regular/15":
    "783441abb39584ab370f14bd69a5ca827ff62d8cb8d2e479b6f9e1c727b6f7e9",
  "poppins/regular/18":
    "8c04c7c8ef3dff4ac1c5335f18d758808c50d6578989947a28494815faab1763",
  "poppins/regular/22":
    "9715b592fb75c348817c3703e89045ff17959a7b9ae34eddd12906ede18d52e6",
  "poppins/regular/27":
    "72bc69a53387765068a31bd383b837d1639d4bca39ad2035aa8f057e4dafc8d3",
  "poppins/regular/34":
    "9dd7fe42eebd5e2c11c62b71429fa072b1969b124d4f80132cbe203ef4017879",
  "poppins/bold/11":
    "de7d5edc0c3afb6756b85eef7857becd26db193f7cdc6db01bc64fc37d3f6633",
  "poppins/bold/13":
    "119b3c68c0842bc75a1b68dbdd7e9feb6c7e209772a0351149a6a88b2e502fb6",
  "poppins/bold/15":
    "04016856f998ea07df1ea9256b29ca421bd06014aa3dbd15cdca423f68bc55ae",
  "poppins/bold/18":
    "a537010a01aa38a778ea7d8455a83df6a98f69ec977b549493f2e116f93c7631",
  "poppins/bold/22":
    "0f5962a0291a54d29d1c489e0e2e04286894c66fa311264f108d7a85110aae85",
  "poppins/bold/27":
    "56893cdbba357b7e0a3220855eda67233439bda9f81b7518ccbf55d81471b97f",
  "poppins/bold/34":
    "815b49e8d1ee908cdfcae9713fb5711737ceab22e821709d526ee26c6302fe84",
};

function digest(fb: FrameBuffer): string {
  return sha256HexSync(pack(fb));
}

function specimenFrame(
  family: (typeof FONT_FAMILY_IDS)[number],
  weight: (typeof FONT_WEIGHTS)[number],
  size: (typeof FONT_SIZES)[number],
): FrameBuffer {
  const fb = new FrameBuffer(WHITE);
  fb.drawText(font(family, weight, size), 2, 2, SPECIMEN, BLACK);
  return fb;
}

describe("font atlases", () => {
  it("ships a licence and provenance for every family, and no others", () => {
    expect(Object.keys(FONT_FAMILY_META).sort()).toEqual([...FONT_FAMILY_IDS].sort());
    for (const family of FONT_FAMILY_IDS) {
      const meta = FONT_FAMILY_META[family];
      // Every face on the panel has to be redistributable, because its
      // rasterised glyphs are committed to this repository.
      expect(meta?.licence).toBe("SIL Open Font License 1.1");
      expect(meta?.licence_file).toMatch(/^assets\/fonts\/.+\/OFL\.txt$/);
      expect(meta?.copyright.length).toBeGreaterThan(0);
      for (const weight of FONT_WEIGHTS) {
        // A real face per weight. Nothing is synthesised from another.
        expect(meta?.faces[weight]?.sha256).toMatch(/^[0-9a-f]{64}$/);
      }
    }
  });

  it("has no glyph with a fractional advance", () => {
    // Whole-pixel advances are the other half of the hinting fix: a
    // fractional pen drifts off the grid between letters and undoes it.
    for (const family of FONT_FAMILY_IDS) {
      for (const weight of FONT_WEIGHTS) {
        for (const size of FONT_SIZES) {
          for (const glyph of Object.values(font(family, weight, size).glyphs)) {
            expect(Number.isInteger(glyph.a)).toBe(true);
          }
        }
      }
    }
  });

  it("gets wider and taller as the size ladder climbs", () => {
    for (const family of FONT_FAMILY_IDS) {
      for (const weight of FONT_WEIGHTS) {
        let lastWidth = 0;
        let lastHeight = 0;
        for (const size of FONT_SIZES) {
          const atlas = font(family, weight, size);
          const width = measureText(atlas, SPECIMEN);
          expect(width).toBeGreaterThan(lastWidth);
          expect(atlas.lineHeight).toBeGreaterThanOrEqual(lastHeight);
          lastWidth = width;
          lastHeight = atlas.lineHeight;
        }
      }
    }
  });

  it("draws bold heavier than regular at the same size", () => {
    const ink = (fb: FrameBuffer): number =>
      fb.pixels.reduce((n, v) => (v === BLACK ? n + 1 : n), 0);
    for (const family of FONT_FAMILY_IDS) {
      for (const size of FONT_SIZES) {
        expect(ink(specimenFrame(family, "bold", size))).toBeGreaterThan(
          ink(specimenFrame(family, "regular", size)),
        );
      }
    }
  });

  describe("golden specimen per family, weight and size", () => {
    for (const family of FONT_FAMILY_IDS) {
      for (const weight of FONT_WEIGHTS) {
        for (const size of FONT_SIZES) {
          const key = `${family}/${weight}/${size}`;
          it(`${key} matches its golden frame`, () => {
            expect(digest(specimenFrame(family, weight, size))).toBe(
              FONT_GOLDEN[key],
            );
          });
        }
      }
    }
  });

  it("renders the same bytes every time it is asked", () => {
    const once = digest(specimenFrame("inter", "regular", 15));
    expect(digest(specimenFrame("inter", "regular", 15))).toBe(once);
  });
});

describe("text style schema", () => {
  it("defaults to something readable rather than something small", () => {
    const style = TextStyleSchema.parse({});
    expect(style.size).toBe(15);
    expect(style.family).toBe("inter");
    expect(style.weight).toBe("regular");
    expect(style.align).toBe("left");
  });

  it("refuses a size that is not on the ladder, and says why", () => {
    // 12 px is between 11 and 13 and has no committed atlas. Accepting it
    // would mean promising a size the renderer would then throw on.
    const result = TextStyleSchema.safeParse({ size: 12 });
    expect(result.success).toBe(false);
    if (!result.success) {
      expect(result.error.issues[0]?.message).toMatch(/committed glyph atlas/);
    }
    expect(TextStyleSchema.safeParse({ size: 9 }).success).toBe(false);
    expect(TextStyleSchema.safeParse({ size: 48 }).success).toBe(false);
    for (const size of FONT_SIZES) {
      expect(TextStyleSchema.safeParse({ size }).success).toBe(true);
    }
  });

  it("bounds line spacing so text cannot be flung out of its tile", () => {
    expect(TextStyleSchema.safeParse({ lineSpacing: -1 }).success).toBe(false);
    expect(TextStyleSchema.safeParse({ lineSpacing: 13 }).success).toBe(false);
    expect(TextStyleSchema.safeParse({ lineSpacing: 12 }).success).toBe(true);
  });

  it("refuses a family with no committed atlas", () => {
    expect(TextStyleSchema.safeParse({ family: "helvetica" }).success).toBe(false);
  });

  it("enforces the character ceiling a role declares", () => {
    const schema = textElementSchema({ text: "hi", maxLength: 5 });
    expect(schema.safeParse({ text: "12345" }).success).toBe(true);
    expect(schema.safeParse({ text: "123456" }).success).toBe(false);
  });
});

describe("wrapping", () => {
  const atlas = font("inter", "regular", 15);

  it("breaks on words and never exceeds the width", () => {
    const { lines, hardBroken } = wrapText(
      atlas,
      "Collect the parcel before noon and lock the side gate",
      140,
    );
    expect(lines.length).toBeGreaterThan(1);
    expect(hardBroken).toBe(false);
    for (const line of lines) {
      expect(measureText(atlas, line)).toBeLessThanOrEqual(140);
    }
    expect(lines.join(" ")).toBe(
      "Collect the parcel before noon and lock the side gate",
    );
  });

  it("keeps a short string on one line", () => {
    expect(wrapText(atlas, "court", 200).lines).toEqual(["court"]);
  });

  it("breaks a single over-wide word rather than dropping it, and says so", () => {
    const { lines, hardBroken } = wrapText(atlas, "anticonstitutionnellement", 40);
    expect(hardBroken).toBe(true);
    expect(lines.length).toBeGreaterThan(1);
    // Every character survives the break; nothing is silently discarded.
    expect(lines.join("")).toBe("anticonstitutionnellement");
  });

  it("collapses whitespace before wrapping", () => {
    expect(wrapText(atlas, "  deux   mots  ", 400).lines).toEqual(["deux mots"]);
  });

  it("wraps to more lines as the size climbs the ladder", () => {
    const text = "Collect the parcel before noon and lock the side gate";
    const small = wrapText(font("inter", "regular", 11), text, 140).lines.length;
    const large = wrapText(font("inter", "regular", 27), text, 140).lines.length;
    expect(large).toBeGreaterThan(small);
  });
});

describe("overflow", () => {
  const box = { x: 0, y: 0, w: 100, h: 40 };
  const element = (over: Partial<TextElement> = {}): TextElement => ({
    text: "Un texte beaucoup trop long pour cette boîte",
    visible: true,
    style: TextStyleSchema.parse({}),
    ...over,
  });

  it("reports a width overflow instead of truncating in silence", () => {
    const laid = layoutText(element(), box, "body", {});
    expect(laid?.overflow).toHaveLength(1);
    expect(laid?.overflow[0]?.kind).toBe("width");
    expect(laid?.overflow[0]?.neededPx).toBeGreaterThan(100);
    expect(laid?.overflow[0]?.availablePx).toBe(100);
    // The full text travels with the warning so the designer can quote it.
    expect(laid?.overflow[0]?.text).toContain("beaucoup trop long");
  });

  it("reports a height overflow when wrapped lines do not fit", () => {
    const laid = layoutText(element(), { ...box, h: 20 }, "body", { wrap: true });
    expect(laid?.overflow.some((o) => o.kind === "height")).toBe(true);
    expect(laid?.visibleLines).toBeLessThan(laid?.lines.length ?? 0);
  });

  it("reports nothing when the text fits", () => {
    const laid = layoutText(element({ text: "court" }), box, "body", {});
    expect(laid?.overflow).toEqual([]);
  });

  it("marks overflowing text on the panel in the attention colour", () => {
    const fb = new FrameBuffer(WHITE);
    const result = drawText(fb, box, element(), BLACK, "body");
    expect(result.overflow).toHaveLength(1);

    // The mark is real ink on the real frame, not only a designer warning:
    // e-paper holds its image for hours whether or not anyone was watching
    // the designer when it was pushed.
    const red = fb.pixels.reduce((n, v) => (v === RED ? n + 1 : n), 0);
    expect(red).toBeGreaterThan(0);
  });

  it("draws no mark when nothing overflowed", () => {
    const fb = new FrameBuffer(WHITE);
    drawText(fb, box, element({ text: "court" }), BLACK, "body");
    expect(fb.pixels.reduce((n, v) => (v === RED ? n + 1 : n), 0)).toBe(0);
  });

  it("marks a box where not one line fitted, which used to be silent", () => {
    // A 34 px line in a 20 px box: nothing is drawn at all, and a blank tile
    // on frozen ink reads as "there was nothing to say" rather than as "this
    // did not fit". The mark is the difference between those two sentences.
    const fb = new FrameBuffer(WHITE);
    const tiny = { x: 0, y: 0, w: 100, h: 20 };
    const big = element({
      text: "1234",
      style: TextStyleSchema.parse({ size: 34 }),
    });
    const result = drawText(fb, tiny, big, BLACK, "value", { wrap: true });

    expect(result.overflow.some((fact) => fact.kind === "height")).toBe(true);
    const red = fb.pixels.reduce((n, v) => (v === RED ? n + 1 : n), 0);
    expect(red).toBeGreaterThan(0);
    // And it is inside the box it belongs to, not spilling into the next tile.
    for (let y = tiny.h; y < 300; y += 1) {
      expect(fb.get(tiny.x + tiny.w - 1, y)).toBe(WHITE);
    }
  });

  it("hands the warning to the module reporter", () => {
    const seen: string[] = [];
    const fb = new FrameBuffer(WHITE);
    drawText(fb, box, element(), BLACK, "heading", {
      reporter: { overflow: (fact) => seen.push(`${fact.role}/${fact.kind}`) },
    });
    expect(seen).toEqual(["heading/width"]);
  });

  it("carries the module id up to the dashboard report", () => {
    const doc = starterDashboard("Kitchen panel");
    const target = doc.modules.find((m) => m.type === "message");
    expect(target).toBeDefined();
    if (!target) return;
    target.options = moduleDefinition("message").schema.parse({
      body: {
        text: "Un message beaucoup trop long pour tenir dans cette petite tuile",
        style: { size: 27 },
      },
    }) as Record<string, unknown>;

    const { report } = renderDashboardWithReport(
      doc,
      fixtureSources(),
      FIXTURE_CTX,
    );
    const mine = report.overflows.filter((o) => o.moduleId === target.id);
    expect(mine.length).toBeGreaterThan(0);
    expect(mine[0]?.moduleType).toBe("message");
  });

  it("the starter dashboard fits itself with room to spare", () => {
    // Defaults that warn on first run would teach the owner to ignore the warning.
    const { report } = renderDashboardWithReport(
      starterDashboard("Kitchen panel"),
      fixtureSources(),
      FIXTURE_CTX,
    );
    expect(report.overflows).toEqual([]);
  });
});

describe("hiding and deleting", () => {
  it("a hidden module leaves no ink at all", () => {
    const doc = starterDashboard("Kitchen panel");
    const before = renderDashboard(doc, fixtureSources(), FIXTURE_CTX);
    const weather = doc.modules.find((m) => m.type === "weather24h");
    expect(weather).toBeDefined();
    if (!weather) return;
    weather.hidden = true;

    const after = renderDashboard(doc, fixtureSources(), FIXTURE_CTX);
    expect(digest(after)).not.toBe(digest(before));

    // Every pixel of its rectangle is white, and nothing outside it moved.
    const rect = cellsToPixels(weather.x, weather.y, weather.w, weather.h);
    for (let y = rect.y; y < rect.y + rect.h; y += 1) {
      for (let x = rect.x; x < rect.x + rect.w; x += 1) {
        expect(after.get(x, y)).toBe(WHITE);
      }
    }
  });

  it("hiding keeps the module, its options and its place", () => {
    const doc = starterDashboard("Kitchen panel");
    const weather = doc.modules.find((m) => m.type === "weather24h");
    if (!weather) return;
    const options = JSON.stringify(weather.options);
    weather.hidden = true;
    expect(doc.modules).toHaveLength(6);
    expect(JSON.stringify(weather.options)).toBe(options);
    expect(weather.x).toBe(0);
  });

  it("deleting removes it from the frame and the document", () => {
    const doc = starterDashboard("Kitchen panel");
    const weather = doc.modules.find((m) => m.type === "weather24h");
    if (!weather) return;
    const hidden = { ...doc, modules: doc.modules.map((m) => (m.id === weather.id ? { ...m, hidden: true } : m)) };
    const deleted = { ...doc, modules: doc.modules.filter((m) => m.id !== weather.id) };

    expect(deleted.modules).toHaveLength(5);
    // Hiding and deleting paint the same panel. The difference is only that
    // one of them can be undone without retyping anything.
    expect(digest(renderDashboard(deleted, fixtureSources(), FIXTURE_CTX))).toBe(
      digest(renderDashboard(hidden, fixtureSources(), FIXTURE_CTX)),
    );
  });

  it("an invisible text element draws nothing and reserves no height", () => {
    const fb = new FrameBuffer(WHITE);
    const element: TextElement = {
      text: "LES PROCHAINES 24H",
      visible: false,
      style: TextStyleSchema.parse({}),
    };
    const result = drawText(fb, { x: 0, y: 4, w: 200, h: 40 }, element, BLACK, "heading");
    expect(result.nextY).toBe(4);
    expect(reservedHeight(element)).toBe(0);
    expect(fb.pixels.every((v) => v === WHITE)).toBe(true);
  });

  it("an empty text element reserves no height either", () => {
    expect(
      reservedHeight({ text: "   ", visible: true, style: TextStyleSchema.parse({}) }),
    ).toBe(0);
  });
});

describe("provenance", () => {
  it("is off by default on every module that offers it", () => {
    for (const type of MODULE_TYPES) {
      const options = moduleDefinition(type).defaultOptions as {
        showProvenance?: unknown;
      };
      if ("showProvenance" in options) {
        expect(options.showProvenance).toBe(false);
      }
    }
  });

  it("keeps the source's own label off the panel unless asked", () => {
    const definition = moduleDefinition("weather24h");
    const render = (showProvenance: boolean): FrameBuffer => {
      const fb = new FrameBuffer(WHITE);
      definition.render(
        fb,
        cellsToPixels(0, 0, 6, 3),
        WEATHER_OK as unknown as ModuleData<never>,
        definition.schema.parse({ showProvenance }),
        FIXTURE_CTX,
      );
      return fb;
    };
    const off = render(false);
    const on = render(true);
    expect(digest(off)).not.toBe(digest(on));

    // The provenance line is longer than the heading, so turning it on adds
    // ink. Turning it off is what removes "<place> · Open-Meteo".
    const ink = (fb: FrameBuffer): number =>
      fb.pixels.reduce((n, v) => (v !== WHITE ? n + 1 : n), 0);
    expect(ink(on)).toBeGreaterThan(ink(off));
  });

  it("is not a free-text field, so the panel cannot claim a source", () => {
    // A writable provenance string would let the panel name a source that did
    // not supply the number. Only its typography is settable.
    const shape = moduleDefinition("weather24h").defaultOptions as Record<
      string,
      unknown
    >;
    expect(shape.showProvenance).toBe(false);
    expect(shape.provenanceStyle).toMatchObject({ size: 11 });
    expect(shape).not.toHaveProperty("provenanceText");
  });
});

describe("templates", () => {
  it("substitutes known placeholders and leaves unknown ones standing", () => {
    expect(fillTemplate("{low} à {high} {unit}", { low: 11, high: 22, unit: "°C" })).toBe(
      "11 à 22 °C",
    );
    // A typo shows up on the panel instead of becoming a missing number.
    expect(fillTemplate("{hihg}", { high: 22 })).toBe("{hihg}");
  });

  it("lets the words go while the values stay", () => {
    expect(fillTemplate("{low}/{high}", { low: 1, high: 2 })).toBe("1/2");
    expect(fillTemplate("", { low: 1 })).toBe("");
  });
});

describe("migration from schema 1", () => {
  function v1Doc(modules: Array<Record<string, unknown>>): Record<string, unknown> {
    return {
      schema_version: 1,
      id: "d_legacy",
      title: "Legacy",
      status: "active",
      grid: { cols: 8, rows: 6 },
      modules,
      createdAt: "2026-01-01T00:00:00.000Z",
      updatedAt: "2026-01-01T00:00:00.000Z",
    };
  }

  it("lifts a whole document and keeps its identity", () => {
    const migrated = migrateDashboardDoc(
      v1Doc([
        {
          id: "m0",
          type: "message",
          x: 0,
          y: 0,
          w: 6,
          h: 1,
          options: { text: "Collect the parcel", expiresAt: null },
        },
      ]),
    ) as { schema_version: number; id: string; createdAt: string; modules: Array<Record<string, unknown>> };

    expect(migrated.schema_version).toBe(DASHBOARD_SCHEMA_VERSION);
    expect(migrated.id).toBe("d_legacy");
    expect(migrated.createdAt).toBe("2026-01-01T00:00:00.000Z");
    expect(migrated.modules[0]?.hidden).toBe(false);
    // And the result is a document this build actually accepts.
    expect(() => parseDashboard(migrated)).not.toThrow();
  });

  it("keeps every word the user had written", () => {
    const options = migrateModuleOptions("message", {
      text: "Collect the parcel before noon",
      expiresAt: "2030-01-01T00:00:00.000Z",
    }) as { body: TextElement; expiresAt: string };
    expect(options.body.text).toBe("Collect the parcel before noon");
    expect(options.body.visible).toBe(true);
    expect(options.expiresAt).toBe("2030-01-01T00:00:00.000Z");
  });

  it("carries a custom heading across", () => {
    const options = migrateModuleOptions("weather24h", {
      heading: "LA MÉTÉO",
      showRange: true,
      showLocation: true,
    }) as { heading: TextElement };
    expect(options.heading.text).toBe("LA MÉTÉO");
  });

  it("does NOT carry the provenance line forward, even when it was on", () => {
    // v1's showLocation defaulted to true and drew
    // "<place> · Open-Meteo". That is the specific line the owner
    // rejected on the panel, so the migration drops it rather than preserving
    // it. This is the one deliberate content change in the whole migration.
    for (const showLocation of [true, false]) {
      const options = migrateModuleOptions("weather24h", {
        heading: "LES PROCHAINES 24H",
        showRange: true,
        showLocation,
      }) as { showProvenance: boolean; location: TextElement };
      expect(options.showProvenance).toBe(false);
      expect(options.location.visible).toBe(false);
      expect(options.location.text).toBe("");
    }
  });

  it("preserves showRange as the range line's visibility", () => {
    for (const showRange of [true, false]) {
      const options = migrateModuleOptions("weather24h", { showRange }) as {
        rangeLine: TextElement;
      };
      expect(options.rangeLine.visible).toBe(showRange);
      expect(options.rangeLine.text).toContain("{low}");
    }
  });

  it("moves the timestamp's prefix into the wording and its align into the style", () => {
    const options = migrateModuleOptions("timestamp", {
      prefix: "MAJ",
      align: "left",
    }) as { label: TextElement };
    expect(options.label.text).toBe("MAJ {stamp}");
    expect(options.label.style.align).toBe("left");

    const right = migrateModuleOptions("timestamp", {
      prefix: "Mis à jour",
      align: "right",
    }) as { label: TextElement };
    expect(right.label.text).toBe("Mis à jour {stamp}");
    expect(right.label.style.align).toBe("right");
  });

  it("turns the octopus caption boolean into the caption's visibility", () => {
    for (const showCaption of [true, false]) {
      const options = migrateModuleOptions("octopus", {
        centre: false,
        showCaption,
      }) as { centre: boolean; caption: TextElement };
      expect(options.centre).toBe(false);
      expect(options.caption.visible).toBe(showCaption);
    }
  });

  it("keeps the sensor's entity and label", () => {
    const options = migrateModuleOptions("haSensor", {
      entityId: "binary_sensor.front_door",
      label: "Porte",
    }) as { entityId: string; label: TextElement };
    expect(options.entityId).toBe("binary_sensor.front_door");
    expect(options.label.text).toBe("Porte");
  });

  it("gives migrated text the new, larger defaults rather than the old sizes", () => {
    // Carrying v1's 12 px forward would migrate the exact rendering that was
    // rejected on the physical panel.
    const options = migrateModuleOptions("calendarNext", {
      heading: "À VENIR",
      maxEvents: 3,
    }) as { heading: TextElement; maxEvents: number };
    expect(options.heading.style.size).toBe(15);
    expect(options.heading.style.weight).toBe("bold");
    expect(options.maxEvents).toBe(3);
  });

  it("produces options its own module schema accepts, for every module", () => {
    for (const type of MODULE_TYPES) {
      const migrated = migrateModuleOptions(type, {});
      expect(moduleDefinition(type).schema.safeParse(migrated).success).toBe(true);
    }
  });

  it("leaves an unknown module type alone instead of inventing a shape", () => {
    expect(migrateModuleOptions("teleporter", { warp: 9 })).toEqual({ warp: 9 });
  });

  it("leaves a document that is already current untouched", () => {
    const current = starterDashboard("Kitchen panel");
    expect(migrateDashboardDoc(current)).toBe(current);
  });
});
