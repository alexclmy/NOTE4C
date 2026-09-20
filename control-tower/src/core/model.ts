import { z } from "zod";
import { GRID_COLS, GRID_ROWS } from "./render/types";
import { MODULES, isKnownModuleType, moduleDefinition } from "./render/modules";
import { migrateDashboardDoc } from "./migrate";
import { DEFAULT_THEME, DashboardThemeSchema } from "./theme";

/**
 * 3 adds the dashboard theme: content padding, dashboard typography and the
 * palette policy. 4 records the refresh interval explicitly. 5 adds the theme's
 * Expression block — colour use, brush, pixel texture, larger text. Every field
 * of every one of these defaults to the value that reproduces what the previous
 * version painted, so each migration is additive and invisible.
 */
export const DASHBOARD_SCHEMA_VERSION = 5;

export const RefreshIntervalSchema = z.union([
  z.null(), z.literal(5), z.literal(15), z.literal(30), z.literal(60), z.literal(180),
]);

export const ModuleInstanceSchema = z.object({
  id: z.string().min(1).max(64),
  type: z.string().min(1).max(64),
  x: z.number().int().min(0).max(GRID_COLS - 1),
  y: z.number().int().min(0).max(GRID_ROWS - 1),
  w: z.number().int().min(1).max(GRID_COLS),
  h: z.number().int().min(1).max(GRID_ROWS),
  /**
   * Hidden modules keep their place on the grid and their options, and draw
   * nothing. This is the difference between "not today" and "gone": deleting
   * a module throws away its wording, and hiding it does not.
   */
  hidden: z.boolean().default(false),
  options: z.record(z.string(), z.unknown()).default({}),
});
export type ModuleInstance = z.infer<typeof ModuleInstanceSchema>;

export const GridSchema = z.object({
  cols: z.literal(GRID_COLS),
  rows: z.literal(GRID_ROWS),
});

export const DashboardDocSchema = z.object({
  schema_version: z.literal(DASHBOARD_SCHEMA_VERSION),
  id: z.string().min(1).max(64),
  title: z.string().min(1).max(80),
  status: z.enum(["active", "archived"]),
  grid: GridSchema,
  /**
   * Dashboard-wide theme. Defaulted rather than required so a version saved
   * before themes existed still parses, and so the default is stored once at
   * the boundary instead of being re-derived at every render.
   */
  theme: DashboardThemeSchema.default({}),
  /** Null keeps all refreshes and pushes manual. */
  refreshIntervalMinutes: RefreshIntervalSchema,
  modules: z.array(ModuleInstanceSchema).max(24),
  createdAt: z.string(),
  updatedAt: z.string(),
});
export type DashboardDoc = z.infer<typeof DashboardDocSchema>;

export class DashboardValidationError extends Error {
  readonly problems: string[];
  constructor(problems: string[]) {
    super(`Dashboard layout is not valid: ${problems.join("; ")}`);
    this.name = "DashboardValidationError";
    this.problems = problems;
  }
}

export function overlaps(a: ModuleInstance, b: ModuleInstance): boolean {
  return (
    a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h
  );
}

/**
 * Structural rules the schema alone cannot express: modules must fit the
 * grid, must not overlap, must be a known type, must respect their declared
 * spans, and must carry options their own schema accepts.
 *
 * Overlap is prevented at the model level rather than resolved at render
 * time, so a saved version can never paint a module the designer did not
 * show.
 */
export function layoutProblems(doc: DashboardDoc): string[] {
  const problems: string[] = [];
  const seenIds = new Set<string>();

  for (const module of doc.modules) {
    if (seenIds.has(module.id)) {
      problems.push(`Duplicate module id "${module.id}"`);
    }
    seenIds.add(module.id);

    if (!isKnownModuleType(module.type)) {
      problems.push(`Unknown module type "${module.type}"`);
      continue;
    }

    const definition = moduleDefinition(module.type);

    if (module.x + module.w > doc.grid.cols || module.y + module.h > doc.grid.rows) {
      problems.push(
        `Module "${module.id}" (${module.type}) runs past the ${doc.grid.cols}x${doc.grid.rows} grid`,
      );
    }
    if (module.w < definition.minSpan.w || module.h < definition.minSpan.h) {
      problems.push(
        `Module "${module.id}" is smaller than ${module.type} allows (minimum ${definition.minSpan.w}x${definition.minSpan.h})`,
      );
    }
    if (module.w > definition.maxSpan.w || module.h > definition.maxSpan.h) {
      problems.push(
        `Module "${module.id}" is larger than ${module.type} allows (maximum ${definition.maxSpan.w}x${definition.maxSpan.h})`,
      );
    }

    const options = definition.schema.safeParse(module.options);
    if (!options.success) {
      problems.push(
        `Module "${module.id}" (${module.type}) options rejected: ${options.error.issues
          .map((issue) => `${issue.path.join(".") || "(root)"} ${issue.message}`)
          .join(", ")}`,
      );
    }
  }

  for (let i = 0; i < doc.modules.length; i += 1) {
    for (let j = i + 1; j < doc.modules.length; j += 1) {
      const a = doc.modules[i] as ModuleInstance;
      const b = doc.modules[j] as ModuleInstance;
      if (overlaps(a, b)) {
        problems.push(`Modules "${a.id}" and "${b.id}" overlap`);
      }
    }
  }

  return problems;
}

export function assertValidDashboard(doc: DashboardDoc): DashboardDoc {
  const problems = layoutProblems(doc);
  if (problems.length > 0) throw new DashboardValidationError(problems);
  return doc;
}

export function parseDashboard(raw: unknown): DashboardDoc {
  return assertValidDashboard(
    DashboardDocSchema.parse(migrateDashboardDoc(raw)),
  );
}

/**
 * Fill each module's options through its own schema so defaults are applied
 * once, at the boundary, rather than re-derived at every render.
 */
export function normalizeDashboard(doc: DashboardDoc): DashboardDoc {
  return {
    ...doc,
    modules: doc.modules.map((module) => {
      if (!isKnownModuleType(module.type)) return module;
      const definition = moduleDefinition(module.type);
      const parsed = definition.schema.safeParse(module.options);
      return { ...module, options: parsed.success ? (parsed.data as Record<string, unknown>) : module.options };
    }),
  };
}

export type DragMode = "move" | "resize";

function clamp(value: number, low: number, high: number): number {
  return Math.max(low, Math.min(high, value));
}

/**
 * Where a drag of dx, dy whole cells would put a module.
 *
 * All the designer's geometry lives here rather than in the component, so the
 * rules are testable without a browser and the same clamps apply however the
 * drag arrives (mouse, touch, or a future keyboard nudge).
 *
 * Returns null when the result would overlap another module: overlap is
 * prevented at the model level, so a colliding drag simply does not land and a
 * saved version can never paint a module the designer did not show.
 */
export function applyDrag(
  doc: DashboardDoc,
  origin: ModuleInstance,
  mode: DragMode,
  dx: number,
  dy: number,
): ModuleInstance | null {
  if (!isKnownModuleType(origin.type)) return null;
  const definition = moduleDefinition(origin.type);

  let next: ModuleInstance;
  if (mode === "move") {
    next = {
      ...origin,
      x: clamp(origin.x + dx, 0, doc.grid.cols - origin.w),
      y: clamp(origin.y + dy, 0, doc.grid.rows - origin.h),
    };
  } else {
    next = {
      ...origin,
      w: clamp(
        origin.w + dx,
        definition.minSpan.w,
        Math.min(definition.maxSpan.w, doc.grid.cols - origin.x),
      ),
      h: clamp(
        origin.h + dy,
        definition.minSpan.h,
        Math.min(definition.maxSpan.h, doc.grid.rows - origin.y),
      ),
    };
  }

  const collides = doc.modules.some(
    (other) => other.id !== next.id && overlaps(other, next),
  );
  return collides ? null : next;
}

/** Does a module of this type fit anywhere free on the grid? */
export function findFreeSlot(
  doc: DashboardDoc,
  span: { w: number; h: number },
): { x: number; y: number } | null {
  for (let y = 0; y + span.h <= doc.grid.rows; y += 1) {
    for (let x = 0; x + span.w <= doc.grid.cols; x += 1) {
      const candidate: ModuleInstance = {
        id: "__probe__",
        type: "__probe__",
        x,
        y,
        w: span.w,
        h: span.h,
        hidden: false,
        options: {},
      };
      if (!doc.modules.some((module) => overlaps(module, candidate))) {
        return { x, y };
      }
    }
  }
  return null;
}

export function newModuleId(): string {
  return `m_${Math.random().toString(36).slice(2, 10)}${Date.now().toString(36).slice(-4)}`;
}

export function newDashboardId(): string {
  return `d_${Math.random().toString(36).slice(2, 10)}${Date.now().toString(36).slice(-4)}`;
}

export function emptyDashboard(title: string, now: Date = new Date()): DashboardDoc {
  const stamp = now.toISOString();
  return {
    schema_version: DASHBOARD_SCHEMA_VERSION,
    id: newDashboardId(),
    title,
    status: "active",
    grid: { cols: GRID_COLS, rows: GRID_ROWS },
    theme: DEFAULT_THEME,
    refreshIntervalMinutes: null,
    modules: [],
    createdAt: stamp,
    updatedAt: stamp,
  };
}

/**
 * A sensible first dashboard: the panel the composer paints today,
 * modularised.
 *
 * The calendar tile is three rows rather than two, because the v2 defaults set
 * event titles at 15 px and wrap them to two lines, and two events at that
 * size need the extra row. Laying it out to fit is better than laying it out
 * to overflow and relying on the warning: defaults that warn on first run
 * teach the reader to ignore warnings.
 *
 * The bottom right 3x1 is left free on purpose, so there is somewhere for the
 * first module the owner adds to land.
 */
export function starterDashboard(title: string, now: Date = new Date()): DashboardDoc {
  const doc = emptyDashboard(title, now);
  const place = (
    type: string,
    x: number,
    y: number,
    w: number,
    h: number,
  ): ModuleInstance => ({
    id: newModuleId(),
    type,
    x,
    y,
    w,
    h,
    hidden: false,
    options: { ...(MODULES[type]?.defaultOptions as Record<string, unknown>) },
  });

  return {
    ...doc,
    modules: [
      place("weather24h", 0, 0, 6, 3),
      place("octopus", 6, 0, 2, 2),
      place("haSensor", 6, 2, 2, 1),
      place("calendarNext", 0, 3, 5, 3),
      place("message", 5, 3, 3, 1),
      place("timestamp", 5, 4, 3, 1),
    ],
  };
}
