"use client";

import { useMemo } from "react";
import type { ModuleInstance } from "@/core/model";
import { emptySources } from "@/core/render/data";
import { MODULE_TYPES, moduleDefinition } from "@/core/render/modules";
import { DEFAULT_THEME, type DashboardTheme } from "@/core/theme";
import { FramePreview } from "./FramePreview";
import { thumbnailDoc } from "./VisualPicker";

/**
 * The component catalogue: every module the panel can draw, shown as a real
 * render of itself rather than as a "+ Label" button.
 *
 * Choosing what to add is a visual act — you recognise a headline, a sky, a
 * list by eye — so the gallery shows the eye what it will get: each tile is the
 * same renderer that packs the device bytes, running over that module's own
 * defaults at the composition's current appearance. A hand-drawn icon would
 * drift from the module the first time its defaults changed; this cannot.
 *
 * The grouping is by what a module is FOR, and the list inside each group is
 * generated from the registry (`MODULE_TYPES`), so a new module appears here
 * with no edit — an unplaced type lands in "More" rather than vanishing.
 *
 * The thumbnails render against `emptySources()` at a fixed clock, which is
 * both what a module draws before any source is configured and what keeps
 * twelve canvases from repainting on every source refresh.
 */

interface Category {
  label: string;
  types: string[];
}

const CATEGORIES: readonly Category[] = [
  { label: "Weather", types: ["weather24h", "octopus", "sky"] },
  { label: "Time & calendar", types: ["calendarNext", "countdown", "timestamp"] },
  { label: "Text & notes", types: ["headline", "message", "list", "conditionalMessage"] },
  { label: "Data & home", types: ["haSensor"] },
  { label: "Image", types: ["image"] },
];

/** Lay a module across enough of the panel that its look reads in a thumbnail. */
function gallerySpan(type: string): { w: number; h: number } {
  const span = moduleDefinition(type).defaultSpan;
  return {
    w: Math.min(8, Math.max(span.w, 6)),
    h: Math.min(6, Math.max(span.h, 3)),
  };
}

export function ComponentGallery({
  theme = DEFAULT_THEME,
  onAdd,
}: {
  theme?: DashboardTheme;
  onAdd: (type: string) => void;
}) {
  const sources = useMemo(() => emptySources(), []);
  const now = useMemo(() => new Date(), []);

  // Every registry type, in category order, with anything uncategorised kept
  // rather than dropped — the registry stays the source of truth.
  const groups = useMemo<Category[]>(() => {
    const placed = new Set(CATEGORIES.flatMap((category) => category.types));
    const leftover = MODULE_TYPES.filter((type) => !placed.has(type));
    const known = CATEGORIES.map((category) => ({
      label: category.label,
      types: category.types.filter((type) => MODULE_TYPES.includes(type as never)),
    })).filter((category) => category.types.length > 0);
    return leftover.length > 0
      ? [...known, { label: "More", types: leftover }]
      : known;
  }, []);

  const docs = useMemo(() => {
    const map = new Map<string, ReturnType<typeof thumbnailDoc>>();
    for (const type of MODULE_TYPES) {
      const span = gallerySpan(type);
      const module: ModuleInstance = {
        id: `gallery-${type}`,
        type,
        x: 0,
        y: 0,
        w: span.w,
        h: span.h,
        hidden: false,
        options: { ...(moduleDefinition(type).defaultOptions as Record<string, unknown>) },
      };
      map.set(type, thumbnailDoc(module, { x: 0, y: 0, w: span.w, h: span.h }, theme));
    }
    return map;
  }, [theme]);

  return (
    <section className="gallery" data-testid="component-gallery" aria-label="Add a component">
      <div className="gallery-head">
        <span className="mono-label">Add a component</span>
        <span className="field-hint">Pick one to drop it in the first free slot.</span>
      </div>

      {groups.map((group) => (
        <div className="gallery-group" key={group.label}>
          <h3 className="gallery-eyebrow">{group.label}</h3>
          <div className="gallery-row">
            {group.types.map((type) => {
              const definition = moduleDefinition(type);
              const span = definition.defaultSpan;
              const doc = docs.get(type);
              if (!doc) return null;
              return (
                <button
                  type="button"
                  key={type}
                  className="gallery-item"
                  onClick={() => onAdd(type)}
                  data-testid={`add-${type}`}
                  title={definition.description}
                >
                  <span className="gallery-thumb">
                    <FramePreview
                      doc={doc}
                      sources={sources}
                      now={now}
                      fluid
                      testId={`add-${type}-canvas`}
                    />
                  </span>
                  <span className="gallery-caption">
                    <span className="gallery-name">{definition.label}</span>
                    <span className="gallery-span">
                      {span.w}×{span.h}
                    </span>
                  </span>
                </button>
              );
            })}
          </div>
        </div>
      ))}
    </section>
  );
}
