import fs from "node:fs";
import { afterEach, beforeEach, describe, expect, it } from "vitest";
import { FrameBuffer, pack } from "@/core/frame";
import { PACKED_BYTES, WHITE, isPaletteIndex } from "@/core/palette";
import { MODULE_TYPES, moduleDefinition } from "@/core/render/modules";
import { cellsToPixels, type ModuleData } from "@/core/render/types";
import { renderDashboard } from "@/core/render";
import {
  DASHBOARD_SCHEMA_VERSION,
  newModuleId,
  parseDashboard,
  starterDashboard,
} from "@/core/model";
import {
  DEFAULT_EXPRESSION,
  DEFAULT_THEME,
  MAX_CONTENT_PADDING,
  applyFamilyToAllStyles,
} from "@/core/theme";
import { readRecord, readVersion, STORE_SCHEMA_VERSION } from "@/server/store/dashboards";
import { paths } from "@/server/store/paths";
import { useTempDataRoot } from "./helpers/tempRoot";
import { fixtureSources, noSources, FIXTURE_CTX } from "./fixtures/render";

/**
 * The acceptance gates this expansion had to hold, in one place.
 *
 * Everything here is a promise made to the device rather than to a user: the
 * frame is 30000 bytes, the pixels are four values, and a dashboard saved by
 * an older build still opens.
 */

describe("every frame is exactly 30000 bytes of four colours", () => {
  const STATES: Array<["ok" | "stale" | "unavailable", ModuleData<unknown>]> = [
    ["ok", { state: "ok" }],
    ["stale", { state: "stale" }],
    ["unavailable", { state: "unavailable" }],
  ];

  it("for every module, at every state, span and size on the ladder", () => {
    for (const type of MODULE_TYPES) {
      const definition = moduleDefinition(type);
      const spans = [
        definition.minSpan,
        definition.defaultSpan,
        definition.maxSpan,
      ];
      for (const span of spans) {
        for (const [, data] of STATES) {
          const fb = new FrameBuffer(WHITE);
          definition.render(
            fb,
            cellsToPixels(0, 0, span.w, span.h),
            data as ModuleData<never>,
            definition.defaultOptions,
            { ...FIXTURE_CTX, sources: fixtureSources() },
          );
          // One assertion for the whole buffer: 120000 separate expect()
          // calls per frame would take minutes and say the same thing.
          let offPalette = -1;
          for (let i = 0; i < fb.pixels.length; i += 1) {
            if (!isPaletteIndex(fb.pixels[i] as number)) {
              offPalette = i;
              break;
            }
          }
          expect(offPalette, `${type} ${span.w}x${span.h}`).toBe(-1);
          expect(pack(fb)).toHaveLength(PACKED_BYTES);
        }
      }
    }
  });

  it("for the starter dashboard at every padding the schema allows", () => {
    for (let padding = 0; padding <= MAX_CONTENT_PADDING; padding += 1) {
      const doc = {
        ...starterDashboard("Kitchen panel"),
        theme: { ...DEFAULT_THEME, contentPadding: padding },
      };
      expect(pack(renderDashboard(doc, fixtureSources(), FIXTURE_CTX))).toHaveLength(
        PACKED_BYTES,
      );
      expect(pack(renderDashboard(doc, noSources(), FIXTURE_CTX))).toHaveLength(
        PACKED_BYTES,
      );
    }
  });

  it("for every palette policy, including both accents off", () => {
    for (const red of [true, false]) {
      for (const yellow of [true, false]) {
        const doc = {
          ...starterDashboard("Kitchen panel"),
          theme: {
            ...DEFAULT_THEME,
            palette: { black: true as const, white: true as const, red, yellow },
          },
        };
        const frame = renderDashboard(doc, fixtureSources(), FIXTURE_CTX);
        expect(pack(frame)).toHaveLength(PACKED_BYTES);
        for (const value of frame.pixels) {
          if (!red) expect(value).not.toBe(3);
          if (!yellow) expect(value).not.toBe(2);
        }
      }
    }
  });

  it("for a dashboard of the three new modules, with a global font applied", () => {
    const doc = starterDashboard("Kitchen panel");
    doc.modules = [
      {
        id: newModuleId(),
        type: "list",
        x: 0,
        y: 0,
        w: 4,
        h: 4,
        hidden: false,
        options: moduleDefinition("list").schema.parse({
          rows: [{ text: "Collect the parcel 🥚" }, { text: "Take the bins out" }],
        }) as Record<string, unknown>,
      },
      {
        id: newModuleId(),
        type: "countdown",
        x: 4,
        y: 0,
        w: 4,
        h: 2,
        hidden: false,
        options: moduleDefinition("countdown").schema.parse({
          targetAt: "2026-12-25T12:00:00.000Z",
        }) as Record<string, unknown>,
      },
      {
        id: newModuleId(),
        type: "conditionalMessage",
        x: 4,
        y: 2,
        w: 4,
        h: 2,
        hidden: false,
        options: moduleDefinition("conditionalMessage").schema.parse({
          body: { text: "Take the bins out 🧹" },
          conditions: [{ kind: "daysOfWeek", days: [0, 1, 2, 3, 4, 5, 6] }],
        }) as Record<string, unknown>,
      },
    ];
    const themed = {
      ...doc,
      theme: {
        ...DEFAULT_THEME,
        contentPadding: 10,
        typography: { family: "poppins" as const, weight: "regular" as const },
      },
      modules: doc.modules.map((module) => ({
        ...module,
        options: applyFamilyToAllStyles(module.options),
      })),
    };
    expect(
      pack(renderDashboard(themed, fixtureSources(), FIXTURE_CTX)),
    ).toHaveLength(PACKED_BYTES);
  });
});

describe("a dashboard saved by the previous build still opens", () => {
  let temp: ReturnType<typeof useTempDataRoot>;

  beforeEach(() => {
    temp = useTempDataRoot();
  });

  afterEach(() => {
    temp.dispose();
  });

  /**
   * A schema 2 record on disk, of exactly the shape this store wrote before
   * the theme existed. The point is the path a dashboard the owner saved yesterday
   * actually takes: read, back up, migrate, rewrite.
   */
  function writeVersion2Dashboard(id: string): void {
    const doc = {
      ...starterDashboard("Kitchen panel"),
      id,
      schema_version: 2,
      createdAt: "2026-09-10T00:00:00.000Z",
      updatedAt: "2026-09-10T00:00:00.000Z",
    } as Record<string, unknown>;
    delete doc.theme;
    doc.modules = (doc.modules as Array<Record<string, unknown>>).map(
      (module, index) => ({ ...module, id: `m${index}` }),
    );

    fs.mkdirSync(paths.dashboardVersionsDir(id), { recursive: true });
    fs.writeFileSync(
      paths.dashboardRecord(id),
      JSON.stringify({
        schema_version: 2,
        doc,
        latestVersion: 1,
        versions: [
          {
            version: 1,
            savedAt: "2026-09-10T00:00:00.000Z",
            note: "Created",
            rolledBackFrom: null,
          },
        ],
      }),
    );
    fs.writeFileSync(
      paths.dashboardVersion(id, 1),
      JSON.stringify({
        schema_version: 2,
        version: 1,
        savedAt: "2026-09-10T00:00:00.000Z",
        note: "Created",
        rolledBackFrom: null,
        doc,
      }),
    );
  }

  it("reads it forward and gives it the inert default theme", () => {
    writeVersion2Dashboard("d_v2");
    const record = readRecord("d_v2");
    expect(record).not.toBeNull();
    if (!record) return;
    expect(record.schema_version).toBe(STORE_SCHEMA_VERSION);
    expect(record.doc.schema_version).toBe(DASHBOARD_SCHEMA_VERSION);
    expect(record.doc.theme).toEqual(DEFAULT_THEME);
    expect(record.doc.modules).toHaveLength(6);
    expect(record.doc.createdAt).toBe("2026-09-10T00:00:00.000Z");
  });

  it("paints it exactly as the previous build did", () => {
    writeVersion2Dashboard("d_v2_pixels");
    const record = readRecord("d_v2_pixels");
    if (!record) throw new Error("no record");

    const expected = {
      ...starterDashboard("Kitchen panel"),
      id: "d_v2_pixels",
      createdAt: "2026-09-10T00:00:00.000Z",
      updatedAt: "2026-09-10T00:00:00.000Z",
    };
    expected.modules = expected.modules.map((module, index) => ({
      ...module,
      id: `m${index}`,
    }));

    expect(pack(renderDashboard(record.doc, fixtureSources(), FIXTURE_CTX))).toEqual(
      pack(renderDashboard(expected, fixtureSources(), FIXTURE_CTX)),
    );
  });

  it("migrates its versions too, keeping their numbers and notes", () => {
    writeVersion2Dashboard("d_v2_versions");
    const version = readVersion("d_v2_versions", 1);
    expect(version.version).toBe(1);
    expect(version.note).toBe("Created");
    expect(version.savedAt).toBe("2026-09-10T00:00:00.000Z");
    expect(version.doc.theme).toEqual(DEFAULT_THEME);
  });

  it("copies the original into backups before rewriting it", () => {
    writeVersion2Dashboard("d_v2_backup");
    readRecord("d_v2_backup");
    const backups = fs.readdirSync(paths.backupsDir());
    expect(backups.some((name) => name.startsWith("record.json"))).toBe(true);
    // And it really is the throwaway root, not anybody's real data.
    expect(temp.root).toContain("note4c-tower-test-");
  });

  it("round-trips a themed dashboard through the document schema", () => {
    const doc = {
      ...starterDashboard("Kitchen panel"),
      theme: {
        contentPadding: 14,
        typography: { family: "poppins" as const, weight: "bold" as const },
        palette: {
          black: true as const,
          white: true as const,
          red: true,
          yellow: false,
        },
      },
    };
    const round = parseDashboard(JSON.parse(JSON.stringify(doc)));
    // A theme saved before Expression existed opens and gains the default
    // Expression block, which is inert, so the panel is unchanged.
    expect(round.theme).toEqual({ ...doc.theme, expression: DEFAULT_EXPRESSION });
  });
});
