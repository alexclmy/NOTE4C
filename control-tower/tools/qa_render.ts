/**
 * Render QA sheets: exactly what the panel would show, as PNG.
 *
 *     npx tsx tools/qa_render.ts <output-dir>
 *
 * Uses the real renderer and the real fixtures, so these files are the same
 * pixels the device would be handed. Writes nothing outside the output
 * directory and talks to nothing.
 */

import fs from "node:fs";
import path from "node:path";
import { FrameBuffer, pack } from "@/core/frame";
import { BLACK, WHITE } from "@/core/palette";
import { framePng } from "@/server/png";
import { starterDashboard, type DashboardDoc } from "@/core/model";
import { renderDashboard, renderDashboardWithReport } from "@/core/render";
import { moduleDefinition, MODULE_TYPES } from "@/core/render/modules";
import { cellsToPixels, type ModuleData } from "@/core/render/types";
import { FONT_FAMILY_IDS, FONT_SIZES, FONT_WEIGHTS } from "@/core/font";
import { font } from "@/core/render/fonts";
import { supportedPictograms } from "@/core/render/pictograms";
import { DEFAULT_THEME, applyFamilyToAllStyles } from "@/core/theme";
import {
  CALENDAR_OK,
  FIXTURE_CTX,
  SENSOR_OK,
  UNAVAILABLE,
  WEATHER_OK,
  fixtureSources,
  noSources,
} from "../tests/unit/fixtures/render";

const outDir = process.argv[2];
if (!outDir) {
  console.error("usage: tsx tools/qa_render.ts <output-dir>");
  process.exit(2);
}
fs.mkdirSync(outDir, { recursive: true });

function write(name: string, fb: FrameBuffer): void {
  fs.writeFileSync(path.join(outDir as string, `${name}.png`), framePng(fb));
  const bytes = pack(fb);
  if (bytes.length !== 30000) throw new Error(`${name} packed to ${bytes.length}`);
  console.log(name);
}

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
    const options = messageModule.options as { body: { text: string } };
    options.body.text = "Collect the parcel before noon";
  }
  return doc;
}

// 1. The starter dashboard, healthy and down.
const healthy = renderDashboardWithReport(fixedDoc(), fixtureSources(), FIXTURE_CTX);
write("dashboard-ok", healthy.frame);
write("dashboard-unavailable", renderDashboard(fixedDoc(), noSources(), FIXTURE_CTX));
console.log(
  `  starter overflow warnings: ${healthy.report.overflows.length}`,
  healthy.report.overflows.map((o) => `${o.moduleType}.${o.role}`).join(", "),
);

// 2. Every module on its own, in each state.
const DATA: Record<string, ModuleData<unknown>> = {
  weather24h: WEATHER_OK as ModuleData<unknown>,
  octopus: WEATHER_OK as ModuleData<unknown>,
  calendarNext: CALENDAR_OK as ModuleData<unknown>,
  haSensor: SENSOR_OK as ModuleData<unknown>,
  message: { state: "ok" },
  timestamp: { state: "ok" },
};

for (const type of MODULE_TYPES) {
  const definition = moduleDefinition(type);
  for (const [label, data] of [
    ["ok", DATA[type] ?? { state: "ok" }],
    ["unavailable", UNAVAILABLE as ModuleData<unknown>],
  ] as const) {
    const fb = new FrameBuffer(WHITE);
    let options = definition.schema.parse({});
    if (type === "message") {
      options = definition.schema.parse({
        body: { text: "Collect the parcel before noon, the dépôt closes early" },
      });
    }
    definition.render(
      fb,
      cellsToPixels(0, 0, definition.defaultSpan.w, definition.defaultSpan.h),
      data as ModuleData<never>,
      options,
      FIXTURE_CTX,
    );
    write(`module-${type}-${label}`, fb);
  }
}

// 3. Every family, weight and size on the ladder, as a specimen sheet. This is
//    the sheet a human looks at to decide whether the type is good enough.
for (const family of FONT_FAMILY_IDS) {
  const fb = new FrameBuffer(WHITE);
  let y = 2;
  for (const size of FONT_SIZES) {
    for (const weight of FONT_WEIGHTS) {
      const atlas = font(family, weight, size);
      if (y + atlas.lineHeight > 300) break;
      fb.drawText(atlas, 2, y, `${size} ${weight[0]} Il1O0 Ç é à 19°C`, 0);
      y += atlas.lineHeight + 1;
    }
  }
  write(`specimen-${family}`, fb);
}

// 4. Overflow, deliberately provoked, so the red mark is visible in QA.
{
  const doc = fixedDoc();
  const messageModule = doc.modules.find((m) => m.type === "message");
  if (messageModule) {
    messageModule.options = moduleDefinition("message").schema.parse({
      body: {
        text: "Ceci est un message beaucoup trop long pour cette tuile, et il ne sera pas coupé en silence",
        style: { size: 22 },
      },
    }) as Record<string, unknown>;
  }
  const { frame, report } = renderDashboardWithReport(
    doc,
    fixtureSources(),
    FIXTURE_CTX,
  );
  write("dashboard-overflow", frame);
  console.log(
    `  provoked overflow warnings: ${report.overflows.length}`,
    JSON.stringify(report.overflows.map((o) => `${o.moduleType}.${o.role}/${o.kind}`)),
  );
}

// 5. A dashboard with a module hidden, to confirm it leaves no ink.
{
  const doc = fixedDoc();
  const weather = doc.modules.find((m) => m.type === "weather24h");
  if (weather) weather.hidden = true;
  write("dashboard-hidden-weather", renderDashboard(doc, fixtureSources(), FIXTURE_CTX));
}

// 6. The three new modules, in the states a human has to look at: default,
//    empty, unavailable, overflowing, and fully configured.
{
  const cases: Array<[string, string, { w: number; h: number }, unknown]> = [
    ["list-default", "list", { w: 4, h: 3 }, {}],
    [
      "list-configured",
      "list",
      { w: 4, h: 3 },
      {
        title: { text: "KITCHEN PANEL" },
        marker: "checkbox",
        rows: [
          { text: "Collect the parcel 🥚" },
          { text: "Water the plants" },
          { text: "Take the bins out" },
          { text: "Not this week", visible: false },
        ],
      },
    ],
    [
      "list-numbered",
      "list",
      { w: 4, h: 3 },
      {
        title: { text: "SHOPPING" },
        marker: "numbered",
        rows: [
          { text: "Coffee" },
          { text: "Oat milk" },
          { text: "Tinfoil" },
        ],
      },
    ],
    [
      "list-overflow",
      "list",
      { w: 4, h: 3 },
      {
        title: { text: "TROP DE CHOSES" },
        maxVisibleRows: 4,
        rows: Array.from({ length: 9 }, (_, i) => ({
          text: `Rangée numéro ${i + 1}`,
        })),
      },
    ],
    ["countdown-unavailable", "countdown", { w: 3, h: 2 }, {}],
    [
      "countdown-configured",
      "countdown",
      { w: 3, h: 2 },
      { label: { text: "AVANT LA RÉCOLTE" }, targetAt: "2026-10-01T12:00:00.000Z" },
    ],
    [
      "countdown-passed",
      "countdown",
      { w: 3, h: 2 },
      { targetAt: "2020-01-01T00:00:00.000Z" },
    ],
    [
      "countdown-overflow",
      "countdown",
      { w: 2, h: 1 },
      {
        targetAt: "2030-12-25T12:00:00.000Z",
        valueText: { style: { size: 34 } },
      },
    ],
    ["conditional-unconfigured", "conditionalMessage", { w: 4, h: 1 }, {}],
    [
      "conditional-true",
      "conditionalMessage",
      { w: 4, h: 1 },
      {
        body: { text: "Take the bins out ce soir" },
        conditions: [{ kind: "daysOfWeek", days: [0, 1, 2, 3, 4, 5, 6] }],
      },
    ],
    [
      "conditional-false",
      "conditionalMessage",
      { w: 4, h: 1 },
      {
        body: { text: "Never shown" },
        fallback: { text: "Condition fausse" },
        whenFalse: "showFallback",
        conditions: [{ kind: "dateRange", from: "1999-01-01", to: "1999-12-31" }],
      },
    ],
  ];

  for (const [name, type, span, options] of cases) {
    const definition = moduleDefinition(type);
    const fb = new FrameBuffer(WHITE);
    definition.render(
      fb,
      cellsToPixels(0, 0, span.w, span.h),
      { state: "ok" } as ModuleData<never>,
      definition.schema.parse(options),
      { ...FIXTURE_CTX, sources: fixtureSources() },
    );
    write(`module-${name}`, fb);
  }
}

// 7. Symbols. The whole vocabulary, plus one the panel has no drawing for, so
//    the fallback is on the QA sheet rather than only in a test.
{
  const fb = new FrameBuffer(WHITE);
  let y = 3;
  for (const group of supportedPictograms()) {
    const atlas = font("inter", "regular", 15);
    fb.drawText(
      atlas,
      3,
      y,
      `${group.category} ${group.entries.map((entry) => entry.emoji).join(" ")}`,
      BLACK,
    );
    y += atlas.lineHeight + 2;
  }
  const big = font("inter", "regular", 27);
  fb.drawText(big, 3, y + 6, "🥚 parcel ⏰ 🦄", BLACK);
  write("symbols", fb);
}

// 8. The theme: padding, a disabled accent, and a global font applied.
{
  write(
    "theme-padding-24",
    renderDashboard(
      { ...fixedDoc(), theme: { ...DEFAULT_THEME, contentPadding: 24 } },
      fixtureSources(),
      FIXTURE_CTX,
    ),
  );

  write(
    "theme-no-red",
    renderDashboard(
      {
        ...fixedDoc(),
        theme: {
          ...DEFAULT_THEME,
          palette: { black: true, white: true, red: false, yellow: true },
        },
      },
      noSources(),
      FIXTURE_CTX,
    ),
  );

  const global = fixedDoc();
  write(
    "theme-poppins-everywhere",
    renderDashboard(
      {
        ...global,
        theme: {
          ...DEFAULT_THEME,
          typography: { family: "poppins", weight: "regular" },
        },
        modules: global.modules.map((module) => ({
          ...module,
          options: applyFamilyToAllStyles(module.options),
        })),
      },
      fixtureSources(),
      FIXTURE_CTX,
    ),
  );
}

// 9. A dashboard built from the three new modules, as a whole panel.
{
  const doc = fixedDoc();
  doc.modules = [
    {
      id: "m_list",
      type: "list",
      x: 0,
      y: 0,
      w: 4,
      h: 4,
      hidden: false,
      options: moduleDefinition("list").schema.parse({
        title: { text: "KITCHEN PANEL" },
        marker: "checkbox",
        rows: [
          { text: "Collect the parcel 🥚" },
          { text: "Take the bins out" },
          { text: "Water the plants" },
          { text: "Vérifier le grillage" },
        ],
      }) as Record<string, unknown>,
    },
    {
      id: "m_countdown",
      type: "countdown",
      x: 4,
      y: 0,
      w: 4,
      h: 2,
      hidden: false,
      options: moduleDefinition("countdown").schema.parse({
        label: { text: "AVANT LA RÉCOLTE" },
        targetAt: "2026-10-01T12:00:00.000Z",
      }) as Record<string, unknown>,
    },
    {
      id: "m_conditional",
      type: "conditionalMessage",
      x: 4,
      y: 2,
      w: 4,
      h: 2,
      hidden: false,
      options: moduleDefinition("conditionalMessage").schema.parse({
        body: { text: "Take the bins out ce soir 🧹" },
        conditions: [{ kind: "daysOfWeek", days: [0, 1, 2, 3, 4, 5, 6] }],
        showRule: true,
        ruleColour: "accent",
      }) as Record<string, unknown>,
    },
    {
      id: "m_stamp",
      type: "timestamp",
      x: 5,
      y: 5,
      w: 3,
      h: 1,
      hidden: false,
      options: moduleDefinition("timestamp").defaultOptions as Record<
        string,
        unknown
      >,
    },
  ];
  const { frame, report } = renderDashboardWithReport(
    doc,
    fixtureSources(),
    FIXTURE_CTX,
  );
  write("dashboard-new-modules", frame);
  console.log(
    `  new-module dashboard warnings: ${report.overflows.length} overflow, ${report.notes.length} notes, ${report.contrasts.length} contrast`,
  );
}
