"use client";

import { useMemo, type ReactNode } from "react";
import { MODULE_TYPES, moduleDefinition } from "@/core/render/modules";
import type { DashboardTheme } from "@/core/theme";

/**
 * The component catalogue: every module the panel can draw, offered as a clean
 * labelled row rather than a shrunken live render.
 *
 * Choosing what to add is a recognition task — you know a headline, a list, a
 * sensor by its shape — so each item leads with a plain line icon that reads at
 * a glance, then its name and the space it takes. A muddy 110 px render of the
 * module's defaults told you less than the word "Headline" and its icon do; the
 * real preview belongs on the board, not in the picker.
 *
 * The grouping is by what a module is FOR, and the list inside each group is
 * generated from the registry (`MODULE_TYPES`), so a new module appears here
 * with no edit — an unplaced type lands in "More" rather than vanishing, and an
 * icon it has no case for falls back to a generic block.
 */

interface Category {
  label: string;
  types: string[];
}

const CATEGORIES: readonly Category[] = [
  { label: "Weather", types: ["weatherHero", "weather24h", "octopus", "sky"] },
  { label: "Time & calendar", types: ["calendarNext", "countdown", "timestamp"] },
  { label: "Text & notes", types: ["headline", "message", "list", "conditionalMessage"] },
  { label: "Data & home", types: ["haSensor"] },
  { label: "Image", types: ["image"] },
];

function Glyph({ children }: { children: ReactNode }) {
  return (
    <svg
      viewBox="0 0 24 24"
      fill="none"
      stroke="currentColor"
      strokeWidth={1.8}
      strokeLinecap="round"
      strokeLinejoin="round"
      aria-hidden="true"
    >
      {children}
    </svg>
  );
}

/** A plain line icon per module type; a generic block for anything new. */
function ModuleIcon({ type }: { type: string }) {
  switch (type) {
    case "weatherHero":
      return (
        <Glyph>
          <circle cx="12" cy="12" r="4.4" fill="currentColor" stroke="none" />
          <path d="M12 2.4v2.4M12 19.2v2.4M2.4 12h2.4M19.2 12h2.4M5 5l1.7 1.7M17.3 17.3 19 19M19 5l-1.7 1.7M6.7 17.3 5 19" />
        </Glyph>
      );
    case "weather24h":
      return (
        <Glyph>
          <circle cx="8.5" cy="8" r="3" />
          <path d="M8.5 1.5v1.6M2 8h1.6M4 3.5l1.1 1.1M13 3.5l-1.1 1.1" />
          <path d="M7 19h9a3.4 3.4 0 0 0 .3-6.78A4.8 4.8 0 0 0 7 13a3.3 3.3 0 0 0 0 6z" />
        </Glyph>
      );
    case "octopus":
      return (
        <Glyph>
          <path d="M5.5 12a6.5 6.5 0 0 1 13 0v2.2a1.8 1.8 0 0 1-3.6 0 1.9 1.9 0 0 0-3.8 0 1.8 1.8 0 0 1-3.6 0 1.8 1.8 0 0 1-1.9 1.8z" />
          <circle cx="10" cy="11" r="0.9" fill="currentColor" stroke="none" />
          <circle cx="14" cy="11" r="0.9" fill="currentColor" stroke="none" />
        </Glyph>
      );
    case "sky":
      return (
        <Glyph>
          <circle cx="12" cy="9.5" r="3.6" />
          <path d="M12 2.5v1.8M12 15.4v0M3.4 9.5H1.6M22.4 9.5h-1.8M5.6 3.1 6.9 4.4M18.4 3.1 17.1 4.4" />
          <path d="M3 20h18" />
        </Glyph>
      );
    case "calendarNext":
      return (
        <Glyph>
          <rect x="4" y="5" width="16" height="15" rx="1.2" />
          <path d="M4 9.5h16M8.5 3v4M15.5 3v4" />
          <rect x="14" y="13" width="3.2" height="3.2" rx="0.4" fill="currentColor" stroke="none" />
        </Glyph>
      );
    case "countdown":
      return (
        <Glyph>
          <path d="M7 3h10M7 21h10" />
          <path d="M8 3c0 4 8 5 8 9s-8 5-8 9M16 3c0 4-8 5-8 9s8 5 8 9" />
        </Glyph>
      );
    case "timestamp":
      return (
        <Glyph>
          <circle cx="12" cy="12" r="8.2" />
          <path d="M12 7.5v5l3.2 2" />
        </Glyph>
      );
    case "headline":
      return (
        <Glyph>
          <path d="M4 6h16" strokeWidth={3.2} />
          <path d="M4 12.5h13M4 17h9" />
        </Glyph>
      );
    case "message":
      return (
        <Glyph>
          <path d="M4 5h16v11H9l-4 4v-4H4z" />
          <path d="M8 9.5h8M8 12.5h5" />
        </Glyph>
      );
    case "list":
      return (
        <Glyph>
          <rect x="3.5" y="5.5" width="3.6" height="3.6" rx="0.5" />
          <rect x="3.5" y="14.5" width="3.6" height="3.6" rx="0.5" />
          <path d="M4.4 7.3l0.8 0.8 1.3-1.5" />
          <path d="M10.5 7.3h10M10.5 16.3h10" />
        </Glyph>
      );
    case "conditionalMessage":
      return (
        <Glyph>
          <path d="M6 3v18" />
          <path d="M6 4.5h11l-2.6 3.4L17 11.5H6z" />
        </Glyph>
      );
    case "haSensor":
      return (
        <Glyph>
          <path d="M3.6 16a8.4 8.4 0 0 1 16.8 0" />
          <path d="M12 16l4.2-3.4" />
          <circle cx="12" cy="16" r="1.3" fill="currentColor" stroke="none" />
        </Glyph>
      );
    case "image":
      return (
        <Glyph>
          <rect x="3.5" y="5" width="17" height="14" rx="1.2" />
          <circle cx="8.6" cy="10" r="1.5" />
          <path d="M4 17.5l4.6-4 3.4 2.8 3-2.2 5 3.9" />
        </Glyph>
      );
    default:
      return (
        <Glyph>
          <rect x="4" y="4" width="16" height="16" rx="1.2" />
          <path d="M4 10h16M10 4v16" />
        </Glyph>
      );
  }
}

export function ComponentGallery({
  onAdd,
}: {
  theme?: DashboardTheme;
  onAdd: (type: string) => void;
}) {
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
              return (
                <button
                  type="button"
                  key={type}
                  className="gallery-item"
                  onClick={() => onAdd(type)}
                  data-testid={`add-${type}`}
                  title={definition.description}
                >
                  <span className="gallery-icon">
                    <ModuleIcon type={type} />
                  </span>
                  <span className="gallery-name">{definition.label}</span>
                  <span className="gallery-span">
                    {span.w}×{span.h}
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
