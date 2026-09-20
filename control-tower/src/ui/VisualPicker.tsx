"use client";

import { useId, useMemo } from "react";
import type { DashboardDoc, ModuleInstance } from "@/core/model";
import type { DashboardSources } from "@/core/render/data";
import { DEFAULT_THEME, type DashboardTheme } from "@/core/theme";
import { FramePreview } from "./FramePreview";

/**
 * A picker whose choices are pictures, not words.
 *
 * A disposition or a colour treatment is a thing you recognise by eye, so the
 * control shows the eye what it will get: each option is a real render of the
 * panel set that way, made by the same renderer that packs the device bytes, so
 * a thumbnail cannot promise a look the panel will not deliver. The label is
 * underneath for confirmation, not for the choosing.
 *
 * It is a radio group: one option is chosen, arrows move between them, the
 * whole tile is a 44 px-plus target, and it wraps rather than scrolling
 * sideways on a narrow screen.
 */

export interface PickerOption<T extends string> {
  value: T;
  label: string;
  /** The dashboard to render as this option's thumbnail. */
  doc: DashboardDoc;
  /** A one-line, plain-language note, shown only for the selected option. */
  hint?: string;
}

export function VisualPicker<T extends string>({
  legend,
  value,
  options,
  sources,
  now,
  onChange,
  testId,
}: {
  legend: string;
  value: T;
  options: PickerOption<T>[];
  sources: DashboardSources;
  now: Date;
  onChange: (value: T) => void;
  testId: string;
}) {
  const groupId = useId();
  const selected = options.find((option) => option.value === value);

  const move = (delta: number): void => {
    const index = options.findIndex((option) => option.value === value);
    if (index < 0) return;
    const next = options[(index + delta + options.length) % options.length];
    if (next) onChange(next.value);
  };

  return (
    <div className="visual-picker" data-testid={testId}>
      <span className="visual-picker-legend" id={`${groupId}-legend`}>
        {legend}
      </span>
      <div
        className="visual-picker-row"
        role="radiogroup"
        aria-labelledby={`${groupId}-legend`}
      >
        {options.map((option) => {
          const active = option.value === value;
          return (
            <button
              type="button"
              key={option.value}
              className="visual-swatch"
              role="radio"
              aria-checked={active}
              aria-label={option.label}
              tabIndex={active ? 0 : -1}
              data-active={active}
              data-testid={`${testId}-${option.value}`}
              onClick={() => onChange(option.value)}
              onKeyDown={(event) => {
                if (event.key === "ArrowRight" || event.key === "ArrowDown") {
                  event.preventDefault();
                  move(1);
                } else if (event.key === "ArrowLeft" || event.key === "ArrowUp") {
                  event.preventDefault();
                  move(-1);
                }
              }}
            >
              <span className="visual-swatch-frame">
                <FramePreview
                  doc={option.doc}
                  sources={sources}
                  now={now}
                  fluid
                  testId={`${testId}-${option.value}-canvas`}
                />
              </span>
              <span className="visual-swatch-label">{option.label}</span>
            </button>
          );
        })}
      </div>
      {selected?.hint && (
        <p className="visual-picker-hint" data-testid={`${testId}-hint`}>
          {selected.hint}
        </p>
      )}
    </div>
  );
}

/**
 * Build a one-module dashboard for a thumbnail, laying the module across most
 * of the panel so its disposition reads at a glance.
 */
export function thumbnailDoc(
  module: ModuleInstance,
  span: { x: number; y: number; w: number; h: number },
  theme: DashboardTheme = DEFAULT_THEME,
): DashboardDoc {
  return {
    schema_version: 5,
    id: "thumb",
    title: "thumb",
    status: "active",
    grid: { cols: 8, rows: 6 },
    theme,
    refreshIntervalMinutes: null,
    modules: [{ ...module, ...span, hidden: false }],
    createdAt: "2026-01-01T00:00:00.000Z",
    updatedAt: "2026-01-01T00:00:00.000Z",
  };
}

/** A memo key that changes whenever anything a thumbnail depends on changes. */
export function useThumbnailKey(...parts: unknown[]): string {
  return useMemo(() => JSON.stringify(parts), [parts]);
}
