import { FrameBuffer, pack } from "@/core/frame";
import { WHITE } from "@/core/palette";
import { sha256Hex } from "@/core/hash";
import type { DashboardDoc, ModuleInstance } from "@/core/model";
import {
  DEFAULT_THEME,
  applyLargerText,
  applyPalettePolicy,
  resolveInheritedStyles,
  type DashboardTheme,
} from "@/core/theme";
import { emptySources, type DashboardSources } from "./data";
import { drawModuleFrame } from "./frame";
import { moduleDefinition } from "./modules";
import { PANEL_TIMEZONE } from "./time";
import { cellsToPixels, type ModuleData, type RenderContext } from "./types";
import type {
  ContrastFact,
  LayoutNote,
  ModuleReporter,
  OverflowFact,
} from "./text";

export * from "./types";
export * from "./data";
export { PANEL_TIMEZONE, formatPanelStamp } from "./time";

/** Pick the data a given module instance should receive. */
export function dataForModule(
  module: ModuleInstance,
  sources: DashboardSources,
): ModuleData<unknown> {
  const definition = moduleDefinition(module.type);
  switch (definition.sourceBinding) {
    case "weather":
      return sources.weather;
    case "calendar":
      return sources.calendar;
    case "reminders":
      return sources.reminders;
    case "haSensor": {
      const entityId = String(
        (module.options as { entityId?: unknown }).entityId ?? "",
      );
      return (
        sources.sensors[entityId] ?? {
          state: "unavailable",
          detail: "No reading for this entity",
        }
      );
    }
    case "none":
    default:
      return { state: "ok" };
  }
}

/** One module's text that did not fit the space the layout gave it. */
export type DashboardOverflow = OverflowFact & {
  moduleId: string;
  moduleType: string;
};

/** One module's colour combination that cannot be read on the panel. */
export type DashboardContrast = ContrastFact & {
  moduleId: string;
  moduleType: string;
};

/** One module's "this control cannot do anything here" observation. */
export type DashboardNote = LayoutNote & {
  moduleId: string;
  moduleType: string;
};

/**
 * Collector for everything a render wants to tell the designer.
 *
 * Rendering never depends on anyone listening: the panel marks overflowing
 * text in ink whether or not a collector was passed. This is the channel that
 * turns that mark into a sentence a human can read before pushing.
 */
export class RenderReport {
  readonly overflows: DashboardOverflow[] = [];
  readonly contrasts: DashboardContrast[] = [];
  readonly notes: DashboardNote[] = [];
  /** Pixels the palette policy moved off a disabled pigment. */
  remappedPixels = 0;

  forModule(moduleId: string, moduleType: string): ModuleReporter {
    return {
      overflow: (fact) => {
        this.overflows.push({ ...fact, moduleId, moduleType });
      },
      contrast: (fact) => {
        this.contrasts.push({ ...fact, moduleId, moduleType });
      },
      note: (fact) => {
        this.notes.push({ ...fact, moduleId, moduleType });
      },
    };
  }
}

export function dashboardTheme(doc: DashboardDoc): DashboardTheme {
  return doc.theme ?? DEFAULT_THEME;
}

export function renderDashboard(
  doc: DashboardDoc,
  sources: DashboardSources = emptySources(),
  ctx: RenderContext = { now: new Date(), timeZone: PANEL_TIMEZONE },
  report?: RenderReport,
): FrameBuffer {
  const fb = new FrameBuffer(WHITE);
  const theme = dashboardTheme(doc);

  for (const module of doc.modules) {
    // A hidden module draws nothing at all, not even its own white clear: the
    // frame already starts white, and clearing would be a promise that this
    // rectangle is spoken for when it is not.
    if (module.hidden) continue;

    const definition = moduleDefinition(module.type);
    const rect = cellsToPixels(
      module.x,
      module.y,
      module.w,
      module.h,
      theme.contentPadding,
    );
    // Options were normalised at the store boundary, but a version saved by an
    // older build can still arrive here; parse defensively so one bad module
    // cannot take the whole frame down.
    const parsed = definition.schema.safeParse(module.options);
    const options = parsed.success ? parsed.data : definition.defaultOptions;
    // Inheritance is resolved once, here, so every measurement below this line
    // sees a concrete family and weight. Larger text, when the dashboard asked
    // for it, is applied in the same pass and on the same concrete styles.
    const resolved = applyLargerText(
      resolveInheritedStyles(options, theme),
      theme.expression.largerText,
    );
    const scoped: RenderContext = {
      ...ctx,
      theme,
      sources,
      ...(report ? { report: report.forModule(module.id, module.type) } : {}),
    };
    definition.render(fb, rect, dataForModule(module, sources), resolved, scoped);

    // Chrome last: the module's own frame rules sit on top of its ink, at the
    // very edge of its rectangle, delimiting it from whatever abuts it.
    if (module.frame && module.frame.edges.length > 0) {
      drawModuleFrame(fb, rect, module.frame);
    }
  }

  // The palette policy is enforced on the finished frame rather than inside
  // each module. That is what makes it total: no module, sprite, or module
  // written later can put a disabled pigment on the panel by forgetting to
  // ask.
  const moved = applyPalettePolicy(fb.pixels, theme.palette);
  if (report) report.remappedPixels = moved;

  return fb;
}

/** Render and collect the warnings in one pass, for the designer. */
export function renderDashboardWithReport(
  doc: DashboardDoc,
  sources: DashboardSources = emptySources(),
  ctx: RenderContext = { now: new Date(), timeZone: PANEL_TIMEZONE },
): { frame: FrameBuffer; report: RenderReport } {
  const report = new RenderReport();
  const frame = renderDashboard(doc, sources, ctx, report);
  return { frame, report };
}

export function renderDashboardPacked(
  doc: DashboardDoc,
  sources?: DashboardSources,
  ctx?: RenderContext,
): Uint8Array {
  return pack(renderDashboard(doc, sources, ctx));
}

/** The context a semantic render uses. See RenderContext.semantic. */
export function semanticContext(now: Date = new Date()): RenderContext {
  return { now, timeZone: PANEL_TIMEZONE, semantic: true };
}

/**
 * Hash the visible semantic content: everything the panel would show except
 * the passage of time. Two dashboards with the same semantic hash paint the
 * same picture, so pushing the second one would only cost a refresh cycle.
 *
 * `now` is passed through rather than frozen, because a module whose CONTENT
 * is a function of the date — the countdown — genuinely paints a different
 * panel tomorrow. Modules whose output is only a clock read chromeNow(), which
 * does freeze, so an ordinary dashboard still hashes the same all day.
 */
export async function semanticHash(
  doc: DashboardDoc,
  sources: DashboardSources = emptySources(),
  now: Date = new Date(),
): Promise<string> {
  return sha256Hex(pack(renderDashboard(doc, sources, semanticContext(now))));
}
