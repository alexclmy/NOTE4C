import { MODULES, isKnownModuleType } from "./render/modules";
import { DEFAULT_EXPRESSION, DEFAULT_THEME } from "./theme";

/**
 * Dashboard document migrations.
 *
 * Schema 1 modelled every visible string as a bare option: a heading was a
 * string, a caption was a boolean, and typography did not exist. Schema 2
 * models each one as a text element with its own wording, visibility and
 * type. Schema 3 adds the dashboard theme above the modules.
 *
 * The per-module half of that lift lives on each module definition, as
 * `migrateOptions`, next to the schema it has to satisfy. This file only
 * walks the document and dispatches, so adding a module still means touching
 * exactly one file.
 *
 * Two things change on purpose rather than being carried forward, and both
 * were decisions made against the physical panel:
 *
 *   * TYPOGRAPHY. A v1 heading drawn at 12 px comes out of this migration at
 *     the v2 default for that role, which is larger. Carrying the old sizes
 *     forward would migrate the exact rendering the owner rejected on the panel.
 *   * WEATHER PROVENANCE. v1's `showLocation` defaulted to true and drew the
 *     source's own label — "<place> · Open-Meteo" — onto the panel. It maps to
 *     `showProvenance: false`, not to its old value, because that line is the
 *     specific thing the owner asked to be rid of.
 *
 * Everything else is preserved exactly: wording, entity ids, expiries,
 * geometry, ids and timestamps.
 */

interface LegacyModule {
  id?: string;
  type?: string;
  options?: unknown;
}

export function migrateModuleOptions(
  type: string,
  legacy: Record<string, unknown>,
): Record<string, unknown> {
  if (!isKnownModuleType(type)) {
    // An unknown module type is left exactly as it was. layoutProblems()
    // reports it honestly rather than this function inventing a shape.
    return legacy;
  }
  const definition = MODULES[type];
  const migrated = definition?.migrateOptions?.(legacy) ?? legacy;
  // Parse through the module's own schema so every default is filled once,
  // here, rather than being re-derived at every render.
  const parsed = definition?.schema.safeParse(migrated);
  return parsed?.success
    ? (parsed.data as Record<string, unknown>)
    : (definition?.defaultOptions as Record<string, unknown>);
}

/** Lift one v1 document to v2. Pure: it does not touch disk. */
export function migrateDashboardDocV1toV2(raw: unknown): unknown {
  if (typeof raw !== "object" || raw === null) return raw;
  const doc = raw as { modules?: unknown };
  const modules = Array.isArray(doc.modules)
    ? (doc.modules as LegacyModule[])
    : [];

  return {
    ...doc,
    schema_version: 2,
    modules: modules.map((module) => ({
      ...module,
      // Every v1 module was visible; there was no way to hide one.
      hidden: false,
      options: migrateModuleOptions(
        typeof module.type === "string" ? module.type : "",
        (module.options ?? {}) as Record<string, unknown>,
      ),
    })),
  };
}

/**
 * Lift one v2 document to v3: give it the default theme.
 *
 * Nothing else changes, and that is the whole point. Every field of the
 * default theme is the value that reproduces what v2 painted — no padding, all
 * four pigments enabled, and a dashboard font that no role inherits yet —
 * so a dashboard saved before themes existed comes out of this migration
 * rendering the identical frame. The owner's panel changes when they ask it to.
 *
 * A document that somehow already carries a theme keeps it, so running this
 * twice is safe.
 */
export function migrateDashboardDocV2toV3(raw: unknown): unknown {
  if (typeof raw !== "object" || raw === null) return raw;
  const doc = raw as { theme?: unknown };
  return {
    ...doc,
    schema_version: 3,
    theme: doc.theme ?? DEFAULT_THEME,
  };
}

/** Lift one v3 document to v4 with automation explicitly disabled. */
export function migrateDashboardDocV3toV4(raw: unknown): unknown {
  if (typeof raw !== "object" || raw === null) return raw;
  const doc = raw as { refreshIntervalMinutes?: unknown };
  return {
    ...doc,
    schema_version: 4,
    refreshIntervalMinutes: doc.refreshIntervalMinutes ?? null,
  };
}

/**
 * Lift one v4 document to v5: give the theme its Expression block.
 *
 * Expression is the dashboard-wide creative language — colour use, brush, pixel
 * texture, larger text. Its every default is the inert one: only the modules
 * written against the dither engine read it, and `largerText` is off, so a
 * dashboard saved before Expression existed comes out of this migration
 * painting the identical frame. A theme that already carries an expression
 * keeps it, so running this twice is safe, and a document with no theme at all
 * is left for the schema's own default to fill.
 */
export function migrateDashboardDocV4toV5(raw: unknown): unknown {
  if (typeof raw !== "object" || raw === null) return raw;
  const doc = raw as { theme?: unknown };
  if (doc.theme && typeof doc.theme === "object") {
    const theme = doc.theme as Record<string, unknown>;
    return {
      ...doc,
      schema_version: 5,
      theme: {
        ...theme,
        expression: theme.expression ?? DEFAULT_EXPRESSION,
      },
    };
  }
  // No theme at all: leave it for DashboardThemeSchema's own default, which
  // already carries the default expression.
  return { ...doc, schema_version: 5 };
}

/**
 * Migrate a dashboard document from whatever version it was written at to the
 * current one. Unknown or future versions are returned untouched so the
 * caller's schema can reject them with its own message.
 *
 * The steps chain: a v1 document goes through v2 on its way to v3, so there is
 * one path per version rather than one path per pair of versions.
 */
export function migrateDashboardDoc(raw: unknown): unknown {
  if (typeof raw !== "object" || raw === null) return raw;
  const version = (raw as { schema_version?: unknown }).schema_version;
  if (version === 1)
    return migrateDashboardDocV4toV5(
      migrateDashboardDocV3toV4(
        migrateDashboardDocV2toV3(migrateDashboardDocV1toV2(raw)),
      ),
    );
  if (version === 2)
    return migrateDashboardDocV4toV5(
      migrateDashboardDocV3toV4(migrateDashboardDocV2toV3(raw)),
    );
  if (version === 3) return migrateDashboardDocV4toV5(migrateDashboardDocV3toV4(raw));
  if (version === 4) return migrateDashboardDocV4toV5(raw);
  return raw;
}
