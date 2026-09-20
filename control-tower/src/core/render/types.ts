import type { z } from "zod";
import type { FrameBuffer } from "@/core/frame";
import { FRAME_HEIGHT, FRAME_WIDTH } from "@/core/palette";
import type { DashboardTheme } from "@/core/theme";

/** 8 columns x 6 rows of 50 px cells across the 400x300 panel. */
export const GRID_COLS = 8;
export const GRID_ROWS = 6;
export const CELL = 50;

export interface PixelRect {
  x: number;
  y: number;
  w: number;
  h: number;
}

/**
 * Marker a module puts on its layout-variant enum so the inspector renders it
 * as a row of rendered THUMBNAILS instead of a dropdown of words. The whole
 * point of a disposition is what it looks like, so the picker shows that: each
 * choice is a tiny real render of the module set that way. Recognised by the
 * tag rather than by a field name, so a new module that offers dispositions is
 * picked up with no change to the inspector.
 */
export const LAYOUT_VARIANT_TAG = "layout-variant";

/**
 * Marker a module puts on an option that belongs behind the inspector's
 * "Advanced" fold rather than on its first surface. The default editing surface
 * is meant to be short — pick a disposition, edit the words — so the technical
 * knobs (coordinates, a timezone override) carry this tag and the inspector
 * collapses them by default. It is opt-in per field so a module decides what is
 * advanced about itself, and the fold only appears when something is tagged.
 */
export const ADVANCED_OPTION_TAG = "advanced-option";

export interface Span {
  w: number;
  h: number;
}

/**
 * The clock a render uses when it is hashing rather than painting.
 *
 * Rendering at this sentinel makes the chrome that only says "now" produce a
 * constant, so a dashboard whose only change is "time passed" hashes
 * identically and never burns a panel refresh. Same trick as the composer
 * (note4c-dashboard/dashboard.py semantic_hash).
 *
 * It lives here rather than beside renderDashboard so a module can reach it
 * without importing the renderer that calls the module.
 */
export const SEMANTIC_SENTINEL = new Date("2000-01-01T00:00:00.000Z");

/**
 * The usable rectangle inside the frame once the dashboard's padding is taken
 * off. The frame itself is always 400x300: padding moves where content may
 * go, it never changes what the device is handed.
 */
export function contentRect(padding = 0): PixelRect {
  const pad = Math.max(0, Math.trunc(padding));
  return {
    x: pad,
    y: pad,
    w: FRAME_WIDTH - pad * 2,
    h: FRAME_HEIGHT - pad * 2,
  };
}

/**
 * Where a module's cells land in pixels.
 *
 * With no padding this is exactly `x * 50`, which is why every frame this
 * build rendered before padding existed still renders identically: floor(x *
 * 400 / 8) is 50x and floor(y * 300 / 6) is 50y.
 *
 * With padding the content width is divided by flooring each boundary rather
 * than by rounding a cell size, so the eight columns differ by at most one
 * pixel, they tile the content area exactly, and no column is ever cropped or
 * overlapped. Integer arithmetic throughout: the preview and the packed bytes
 * must agree to the pixel.
 */
export function cellsToPixels(
  x: number,
  y: number,
  w: number,
  h: number,
  padding = 0,
): PixelRect {
  const content = contentRect(padding);
  const left = content.x + Math.floor((x * content.w) / GRID_COLS);
  const right = content.x + Math.floor(((x + w) * content.w) / GRID_COLS);
  const top = content.y + Math.floor((y * content.h) / GRID_ROWS);
  const bottom = content.y + Math.floor(((y + h) * content.h) / GRID_ROWS);
  return { x: left, y: top, w: right - left, h: bottom - top };
}

/**
 * The unavailable-data contract. Every module receives this shape and must
 * render its own explicit unavailable or stale state. A module never renders
 * a zero, a dash, or a blank for missing data: the panel is frozen ink and a
 * fabricated value can sit there for hours looking authoritative.
 */
export type SourceState = "ok" | "unavailable" | "stale";

export interface ModuleData<TValue> {
  state: SourceState;
  value?: TValue;
  /** ISO 8601 with offset. When the underlying source last observed reality. */
  observedAt?: string;
  /** Short, secret-free reason shown in Diagnostics, never on the panel. */
  detail?: string;
}

export function ok<T>(value: T, observedAt?: string): ModuleData<T> {
  return { state: "ok", value, ...(observedAt ? { observedAt } : {}) };
}

export function unavailable<T>(detail?: string): ModuleData<T> {
  return { state: "unavailable", ...(detail ? { detail } : {}) };
}

export function stale<T>(
  value: T | undefined,
  observedAt?: string,
  detail?: string,
): ModuleData<T> {
  return {
    state: "stale",
    ...(value !== undefined ? { value } : {}),
    ...(observedAt ? { observedAt } : {}),
    ...(detail ? { detail } : {}),
  };
}

export interface RenderContext {
  /** Render-time clock. */
  now: Date;
  timeZone: string;
  /**
   * This render is being hashed, not shown.
   *
   * Chrome that only states the current time reads `chromeNow(ctx)` instead of
   * `ctx.now`, so it freezes here and a dashboard whose only change is the
   * passing minute keeps its semantic hash. Content whose VALUE is a function
   * of the date — a countdown — deliberately keeps using `ctx.now`, because
   * "three days left" becoming "two days left" is a different panel and is
   * worth a refresh.
   */
  semantic?: boolean;
  /**
   * The dashboard's theme. Optional so a module can still be rendered on its
   * own in a test; the default theme is the one that reproduces this build's
   * output before themes existed.
   */
  theme?: DashboardTheme;
  /**
   * Every source this dashboard collected.
   *
   * A module is normally handed only its own data. The conditional message is
   * the exception: it decides whether to draw at all from the STATE of sources
   * it does not itself display, so it has to be able to see them. Optional, so
   * a module rendered on its own still works and a condition over a source
   * nobody collected reads as unavailable rather than as true.
   */
  sources?: import("./data").DashboardSources;
  /**
   * Where a module reports text that did not fit. Optional because rendering
   * must never depend on somebody listening: the panel marks an overflow in
   * ink whether or not a designer is open to read the warning.
   */
  report?: import("./text").ModuleReporter;
}

/** The clock for chrome that only says "now". See RenderContext.semantic. */
export function chromeNow(ctx: RenderContext): Date {
  return ctx.semantic ? SEMANTIC_SENTINEL : ctx.now;
}

/** Which server-side adapter feeds a module, if any. */
export type SourceKind =
  | "weather"
  | "calendar"
  | "haSensor"
  | "reminders"
  | "none";

export interface ModuleDefinition<TOptions = unknown, TValue = unknown> {
  type: string;
  /** English label for tower chrome. Panel content stays French. */
  label: string;
  description: string;
  /**
   * Options schema. The input side is deliberately loose: option schemas use
   * .default(), which makes their input type optional, and the designer sends
   * partial objects the schema is expected to complete.
   */
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  schema: z.ZodType<TOptions, z.ZodTypeDef, any>;
  defaultOptions: TOptions;
  defaultSpan: Span;
  minSpan: Span;
  maxSpan: Span;
  sourceBinding: SourceKind;
  /**
   * Carry a schema_version 1 options object forward to this module's current
   * shape. Version 1 stored display strings as bare strings and visibility as
   * ad-hoc booleans; version 2 stores them as text elements with typography.
   *
   * Returning a partial object is fine and expected: the result is parsed
   * through `schema`, which fills every default. Anything this does not
   * recognise is dropped rather than guessed at, so a migration can lose a
   * setting but can never invent one.
   */
  migrateOptions?(legacy: Record<string, unknown>): Record<string, unknown>;
  /**
   * Sources this module reads without displaying, given its options.
   *
   * `sourceBinding` answers "what fills this tile". This answers "what else
   * does it need to know", which is how the conditional message can depend on
   * whether the reminders list is reachable without becoming a reminders tile.
   * Collecting a source nobody asked for would be a read of private data for
   * no reason, so this is opt-in per instance rather than per type.
   */
  extraSourceBindings?(options: TOptions): SourceKind[];
  render(
    fb: FrameBuffer,
    rect: PixelRect,
    data: ModuleData<TValue>,
    options: TOptions,
    ctx: RenderContext,
  ): void;
}
