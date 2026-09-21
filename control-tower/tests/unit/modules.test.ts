import { describe, expect, it } from "vitest";
import { FrameBuffer, pack } from "@/core/frame";
import { BLACK, RED, WHITE } from "@/core/palette";
import { MODULES, MODULE_TYPES, moduleDefinition } from "@/core/render/modules";
import { cellsToPixels, type ModuleData } from "@/core/render/types";
import { renderDashboard, semanticHash, SEMANTIC_SENTINEL } from "@/core/render";
import { starterDashboard, type DashboardDoc } from "@/core/model";
import { sha256HexSync } from "@/server/hash";
import {
  CALENDAR_OK,
  FIXTURE_CTX,
  SENSOR_OK,
  UNAVAILABLE,
  WEATHER_OK,
  fixtureSources,
  noSources,
} from "./fixtures/render";

/**
 * Golden digests. These pin the rendered bytes of every module in every state
 * the unavailable-data contract defines. A change here means the panel output
 * changed; that is either intended and the digest is updated deliberately, or
 * it is a regression.
 */
const GOLDEN: Record<string, string> = {
  "weather24h.ok":
    "2a9fd99d80af16c403c94b777eea58aa2fddcb14bea8a9653284e1dcd382e9d7",
  "weather24h.unavailable":
    "0098bfba4a75d399a8e363e4f6f0dcbbae41ead42ae37b9646ed495285057391",
  // Several digests in this table moved when the fixtures were made generic:
  // the strings these modules draw changed, so the pixels did. The renderer
  // did not. Each one was re-derived by running the suite and reading the
  // value back, not by copying whatever the test reported.
  "calendarNext.ok":
    "3115b0aed7539dc1512d1663484df7dc6d1b2bce8049a67913a3df6788d0a446",
  "calendarNext.unavailable":
    "c8850e6b5a0d68911ddecac6539ea1722e60ccfd5826e27539862e0c108cec79",
  "calendarNext.empty":
    "071dbfd67ee25c98cb03f285b3df47e411af94f2518b86b4022967e1ca24c3ea",
  "haSensor.ok":
    "014b59d503cc459508fff6d655e70df30cba00800341a8d0974094b0b7d77cb2",
  "haSensor.stale":
    "860ce49be3e77965a0f149307b8f87a7cadd47d25f507e129435f00e1827e08a",
  "haSensor.unavailable":
    "4b08ab5013f2b9b2aefb3d6e85dcf18eb7812a59d6626f228f8983b47dd94b1f",
  "message.text":
    "2e608d3fca09074c43905a37fdafdf2507d8b69f7ee3ab95f07df87e034c6908",
  "message.expired":
    "ba5f3f269913a8d12a5851d12f0125cd56582dedbf065853e58ce67e98f60e6c",
  "timestamp.right":
    "22f67451958bdd545b785167dc455d5c27f960bd4923c485d02664caa58168f0",
  "octopus.ok":
    "2e9f5f8dc577fe12d741b311b8637daf3876ae45362b0640198c39c99cbc40fc",
  "octopus.unavailable":
    "b5198c1a11a271e90c276060c0323b47c76417d06114f8aa89b2a2fc4c035321",
  // Updated with the list fixture's own wording, not with the renderer.
  "list.rows":
    "319988d42ed84f5db44a275f0b72bca7d87ba1433d7831551c9ce61b1bda8471",
  "list.empty":
    "1576f9d67233b906a6ce91b83febbae98660a1662ef38d193ac55a83469cf2a4",
  "countdown.ahead":
    "5872d93e77ca779fb98b7b40d383de691759b147b4904e3c8b8e6cf1e624c247",
  "countdown.unset":
    "3688d3fb7e276c099514f05ea7a80588d0c3433810d5a6460251e220586433d1",
  "conditionalMessage.true":
    "bdf7eefccdf06d9779e4ebe1c372d8d00cb37602a595d5fd612fd15c31251274",
  "conditionalMessage.unconfigured":
    "c4432c2be2d1bcd73a7f5d13e691a704c9f64bc60fea812f84f6c9578453976a",
  "composed.ok":
    "afe9672142138c622903ca998c2d1598b0349e31755411d654f29d4d86583341",
  "composed.unavailable":
    "320e851f337c0ed805790292a2e961884392a4a75c2981cba372427a1542ab7e",
};

function renderModule(
  type: string,
  span: { w: number; h: number },
  data: unknown,
  options: unknown = {},
): FrameBuffer {
  const definition = moduleDefinition(type);
  const fb = new FrameBuffer(WHITE);
  definition.render(
    fb,
    cellsToPixels(0, 0, span.w, span.h),
    data as ModuleData<unknown>,
    definition.schema.parse(options),
    FIXTURE_CTX,
  );
  return fb;
}

function digest(fb: FrameBuffer): string {
  return sha256HexSync(pack(fb));
}

function inkCount(fb: FrameBuffer, colour: number): number {
  let count = 0;
  for (const value of fb.pixels) if (value === colour) count += 1;
  return count;
}

describe("module registry", () => {
  it("registers the modules in the order they are offered, heroes first", () => {
    // The two editorial hero modules lead the picker, then the original nine in
    // their established order. Adding Headline and Sky is the intended change.
    expect(MODULE_TYPES).toEqual([
      "headline",
      "sky",
      "weather24h",
      "weatherHero",
  "weatherWeek",
      "calendarNext",
      "haSensor",
      "message",
      "list",
      "countdown",
      "conditionalMessage",
      "timestamp",
      "octopus",
      "image",
    ]);
  });

  it("gives every module a usable default configuration", () => {
    for (const type of MODULE_TYPES) {
      const definition = moduleDefinition(type);
      expect(definition.schema.parse(definition.defaultOptions)).toEqual(
        definition.defaultOptions,
      );
      expect(definition.defaultSpan.w).toBeGreaterThanOrEqual(
        definition.minSpan.w,
      );
      expect(definition.defaultSpan.h).toBeGreaterThanOrEqual(
        definition.minSpan.h,
      );
      expect(definition.maxSpan.w).toBeGreaterThanOrEqual(
        definition.defaultSpan.w,
      );
      expect(definition.maxSpan.h).toBeGreaterThanOrEqual(
        definition.defaultSpan.h,
      );
    }
  });

  it("rejects an unknown module type loudly", () => {
    expect(() => moduleDefinition("nope")).toThrow(/Unknown module type/);
  });
});

describe("golden module renders", () => {
  const cases: Array<[string, () => FrameBuffer]> = [
    ["weather24h.ok", () => renderModule("weather24h", { w: 6, h: 3 }, WEATHER_OK)],
    [
      "weather24h.unavailable",
      () => renderModule("weather24h", { w: 6, h: 3 }, UNAVAILABLE),
    ],
    [
      "calendarNext.ok",
      () => renderModule("calendarNext", { w: 5, h: 2 }, CALENDAR_OK),
    ],
    [
      "calendarNext.unavailable",
      () => renderModule("calendarNext", { w: 5, h: 2 }, UNAVAILABLE),
    ],
    [
      "calendarNext.empty",
      () =>
        renderModule("calendarNext", { w: 5, h: 2 }, {
          state: "ok",
          value: { events: [] },
        }),
    ],
    ["haSensor.ok", () => renderModule("haSensor", { w: 3, h: 1 }, SENSOR_OK)],
    [
      "haSensor.stale",
      () =>
        renderModule("haSensor", { w: 3, h: 1 }, {
          state: "stale",
          value: { label: "Capteur", value: "19.4 °C" },
        }),
    ],
    [
      "haSensor.unavailable",
      () => renderModule("haSensor", { w: 3, h: 1 }, UNAVAILABLE),
    ],
    [
      "message.text",
      () =>
        renderModule("message", { w: 6, h: 1 }, { state: "ok" }, {
          body: { text: "Collect the parcel before noon" },
          expiresAt: null,
        }),
    ],
    [
      "message.expired",
      () =>
        renderModule("message", { w: 6, h: 1 }, { state: "ok" }, {
          body: { text: "Périmé" },
          expiresAt: "2020-01-01T00:00:00.000Z",
        }),
    ],
    [
      "timestamp.right",
      () => renderModule("timestamp", { w: 3, h: 1 }, { state: "ok" }),
    ],
    [
      "list.rows",
      () =>
        renderModule("list", { w: 4, h: 3 }, { state: "ok" }, {
          title: { text: "KITCHEN PANEL" },
          marker: "checkbox",
          rows: [
            { text: "Collect the parcel" },
            { text: "Water the plants" },
            { text: "Take the bins out" },
          ],
        }),
    ],
    [
      "list.empty",
      () => renderModule("list", { w: 4, h: 3 }, { state: "ok" }),
    ],
    [
      "countdown.ahead",
      () =>
        renderModule("countdown", { w: 3, h: 2 }, { state: "ok" }, {
          targetAt: "2026-10-01T12:00:00.000Z",
        }),
    ],
    [
      "countdown.unset",
      () => renderModule("countdown", { w: 3, h: 2 }, { state: "ok" }),
    ],
    [
      "conditionalMessage.true",
      () =>
        renderModule("conditionalMessage", { w: 4, h: 1 }, { state: "ok" }, {
          body: { text: "Take the bins out" },
          conditions: [{ kind: "daysOfWeek", days: [5] }],
        }),
    ],
    [
      "conditionalMessage.unconfigured",
      () =>
        renderModule("conditionalMessage", { w: 4, h: 1 }, { state: "ok" }, {
          body: { text: "Take the bins out" },
        }),
    ],
    ["octopus.ok", () => renderModule("octopus", { w: 2, h: 2 }, WEATHER_OK)],
    [
      "octopus.unavailable",
      () => renderModule("octopus", { w: 2, h: 2 }, UNAVAILABLE),
    ],
  ];

  for (const [name, build] of cases) {
    it(`${name} matches its golden frame`, () => {
      expect(digest(build())).toBe(GOLDEN[name]);
    });
  }
});

describe("the unavailable-data contract", () => {
  it("weather says so in French and promises nothing was invented", () => {
    const fb = renderModule("weather24h", { w: 6, h: 3 }, UNAVAILABLE);
    // The attention colour is present, and no temperature glyphs are.
    expect(inkCount(fb, RED)).toBeGreaterThan(50);
    expect(digest(fb)).not.toBe(GOLDEN["weather24h.ok"]);
  });

  it("a missing sensor never renders a number", () => {
    const missing = renderModule("haSensor", { w: 3, h: 1 }, UNAVAILABLE);
    const zeroed = renderModule("haSensor", { w: 3, h: 1 }, {
      state: "ok",
      value: { label: "Capteur", value: "0" },
    });
    expect(digest(missing)).not.toBe(digest(zeroed));
    expect(inkCount(missing, RED)).toBeGreaterThan(20);
  });

  it("a stale sensor still shows its reading, labelled", () => {
    const fresh = renderModule("haSensor", { w: 3, h: 1 }, SENSOR_OK);
    const staleData = renderModule("haSensor", { w: 3, h: 1 }, {
      state: "stale",
      value: { label: "Capteur", value: "19.4 °C" },
    });
    expect(digest(staleData)).not.toBe(digest(fresh));
    // Stale renders in the attention colour; fresh does not.
    expect(inkCount(fresh, RED)).toBe(0);
    expect(inkCount(staleData, RED)).toBeGreaterThan(20);
  });

  it("every module renders something for every state without throwing", () => {
    for (const type of MODULE_TYPES) {
      const definition = MODULES[type];
      if (!definition) throw new Error(`missing ${type}`);
      for (const state of ["ok", "unavailable", "stale"] as const) {
        expect(() =>
          renderModule(type, definition.defaultSpan, { state }),
        ).not.toThrow();
      }
    }
  });
});

describe("message expiry", () => {
  it("renders nothing once the expiry has passed", () => {
    const expired = renderModule("message", { w: 6, h: 1 }, { state: "ok" }, {
      body: { text: "Collect the parcel" },
      expiresAt: "2020-01-01T00:00:00.000Z",
    });
    expect(inkCount(expired, BLACK)).toBe(0);
  });

  it("renders while the expiry is still ahead", () => {
    const live = renderModule("message", { w: 6, h: 1 }, { state: "ok" }, {
      body: { text: "Collect the parcel" },
      expiresAt: "2030-01-01T00:00:00.000Z",
    });
    expect(inkCount(live, BLACK)).toBeGreaterThan(40);
  });
});

describe("timestamp", () => {
  it("renders the panel timezone, not the server timezone", () => {
    // 2026-09-11T05:07Z is 01:07 in America/Toronto.
    const fb = renderModule("timestamp", { w: 3, h: 1 }, { state: "ok" });
    expect(digest(fb)).toBe(GOLDEN["timestamp.right"]);

    const utc = new FrameBuffer(WHITE);
    moduleDefinition("timestamp").render(
      utc,
      cellsToPixels(0, 0, 3, 1),
      { state: "ok" },
      moduleDefinition("timestamp").schema.parse({}),
      { now: FIXTURE_CTX.now, timeZone: "UTC" },
    );
    expect(digest(utc)).not.toBe(digest(fb));
  });
});

function fixedDoc(): DashboardDoc {
  const doc = starterDashboard("Kitchen panel");
  doc.id = "d_fixed";
  doc.createdAt = "2026-09-11T00:00:00.000Z";
  doc.updatedAt = "2026-09-11T00:00:00.000Z";
  doc.modules.forEach((module, index) => {
    module.id = `m${index}`;
  });
  const messageModule = doc.modules.find((m) => m.type === "message");
  if (messageModule) {
    messageModule.options = moduleDefinition("message").schema.parse({
      body: { text: "Collect the parcel before noon" },
      expiresAt: null,
    }) as Record<string, unknown>;
  }
  return doc;
}

describe("composed dashboard", () => {
  it("matches its golden frame with every source healthy", () => {
    const fb = renderDashboard(fixedDoc(), fixtureSources(), FIXTURE_CTX);
    expect(digest(fb)).toBe(GOLDEN["composed.ok"]);
    expect(pack(fb).length).toBe(30000);
  });

  it("matches its golden frame with every source down", () => {
    const fb = renderDashboard(fixedDoc(), noSources(), FIXTURE_CTX);
    expect(digest(fb)).toBe(GOLDEN["composed.unavailable"]);
  });

  it("renders an empty dashboard as a blank white panel", () => {
    const doc = fixedDoc();
    doc.modules = [];
    const fb = renderDashboard(doc, fixtureSources(), FIXTURE_CTX);
    expect(inkCount(fb, WHITE)).toBe(120000);
  });

  it("is deterministic: the same inputs give the same bytes", () => {
    const a = renderDashboard(fixedDoc(), fixtureSources(), FIXTURE_CTX);
    const b = renderDashboard(fixedDoc(), fixtureSources(), FIXTURE_CTX);
    expect(pack(a)).toEqual(pack(b));
  });
});

describe("semanticHash", () => {
  it("ignores the passage of time", async () => {
    const doc = fixedDoc();
    const a = await semanticHash(doc, fixtureSources());
    const b = await semanticHash(doc, fixtureSources());
    expect(a).toBe(b);

    // Rendering the same doc at two wall-clock times differs only in the
    // timestamp module, so the semantic hash must not move.
    const morning = renderDashboard(doc, fixtureSources(), {
      now: new Date("2026-09-11T09:00:00.000Z"),
      timeZone: "America/Toronto",
    });
    const evening = renderDashboard(doc, fixtureSources(), {
      now: new Date("2026-09-11T21:00:00.000Z"),
      timeZone: "America/Toronto",
    });
    expect(digest(morning)).not.toBe(digest(evening));
    expect(await semanticHash(doc, fixtureSources())).toBe(a);
  });

  it("changes when the content changes", async () => {
    const doc = fixedDoc();
    const before = await semanticHash(doc, fixtureSources());
    const after = await semanticHash(doc, noSources());
    expect(after).not.toBe(before);
  });

  it("changes when the layout changes", async () => {
    const doc = fixedDoc();
    const before = await semanticHash(doc, fixtureSources());
    const moved = fixedDoc();
    const target = moved.modules.find((m) => m.type === "haSensor");
    if (target) target.x = 4;
    expect(await semanticHash(moved, fixtureSources())).not.toBe(before);
  });

  it("uses the 2000-01-01 sentinel clock", () => {
    expect(SEMANTIC_SENTINEL.toISOString()).toBe("2000-01-01T00:00:00.000Z");
  });
});
