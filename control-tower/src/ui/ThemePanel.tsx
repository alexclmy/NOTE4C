"use client";

import { useMemo, useState } from "react";
import { FONT_FAMILY_META } from "@/core/render/fonts";
import { FONT_FAMILY_IDS, FONT_WEIGHTS, type FontFamilyId, type FontWeight } from "@/core/font";
import {
  BRUSHES,
  BRUSH_LABEL,
  BRUSH_NOTE,
  COLOUR_USES,
  COLOUR_USE_LABEL,
  COLOUR_USE_NOTE,
  MAX_CONTENT_PADDING,
  MIN_CONTENT_PADDING,
  OPTIONAL_PIGMENTS,
  PIXEL_TEXTURES,
  PIXEL_TEXTURE_LABEL,
  PIXEL_TEXTURE_NOTE,
  STRUCTURAL_PIGMENTS,
  applyFamilyToAllStyles,
  countStyleInheritance,
  fallbackExplanation,
  type Brush,
  type ColourUse,
  type DashboardTheme,
  type Expression,
  type PixelTexture,
} from "@/core/theme";
import { contentRect } from "@/core/render/types";
import type { DashboardDoc, ModuleInstance } from "@/core/model";
import { Banner, Button, Card } from "./components";
import { ConfirmDialog } from "./Dialog";
import { Fold } from "./Disclosure";
import { FramePreview } from "./FramePreview";
import { VisualPicker, thumbnailDoc, type PickerOption } from "./VisualPicker";
import type { DashboardSources } from "@/core/render/data";

/**
 * Dashboard-wide theme.
 *
 * Two things here are deliberately awkward, because the easy versions would be
 * dishonest.
 *
 *   * Choosing a dashboard font changes nothing on its own. Every text role
 *     starts as its own override, and making them follow is a separate,
 *     confirmed action with a preview, because it overwrites choices the owner made
 *     one tile at a time.
 *   * Turning a pigment off does not remove it from the panel. The panel is
 *     physical; the ink is in it. What the switch does is tell the renderer to
 *     stop asking for that colour, and the words here say which colour it will
 *     draw instead.
 */

const WEIGHT_LABEL: Record<string, string> = {
  regular: "Regular",
  bold: "Bold",
};

export function ThemePanel({
  doc,
  sources,
  now,
  onChange,
}: {
  doc: DashboardDoc;
  sources: DashboardSources;
  now: Date;
  onChange: (next: DashboardDoc) => void;
}) {
  const theme = doc.theme;
  const [confirmApply, setConfirmApply] = useState(false);

  const inheritance = useMemo(() => {
    let total = 0;
    let inheriting = 0;
    for (const module of doc.modules) {
      const counted = countStyleInheritance(module.options);
      total += counted.total;
      inheriting += counted.inheriting;
    }
    return { total, inheriting, overridden: total - inheriting };
  }, [doc.modules]);

  /** What the dashboard would look like if every role followed the theme. */
  const previewDoc = useMemo<DashboardDoc>(
    () => ({
      ...doc,
      modules: doc.modules.map((module) => ({
        ...module,
        options: applyFamilyToAllStyles(module.options),
      })),
    }),
    [doc],
  );

  const setTheme = (patch: Partial<DashboardTheme>): void => {
    onChange({ ...doc, theme: { ...theme, ...patch } });
  };

  const content = contentRect(theme.contentPadding);
  const explanations = fallbackExplanation(theme.palette);

  const expr = theme.expression;
  const setExpression = (patch: Partial<Expression>): void => {
    onChange({ ...doc, theme: { ...theme, expression: { ...expr, ...patch } } });
  };

  // A small, fixed headline set the way this dashboard is, re-skinned for each
  // swatch by overriding one expression field. What the swatch shows is what
  // the renderer would draw, so the picker cannot promise a look the panel
  // will not print.
  const sampleModule: ModuleInstance = {
    id: "sample",
    type: "headline",
    x: 0,
    y: 0,
    w: 8,
    h: 3,
    hidden: false,
    options: {
      variant: "underline",
      palette: "warm",
      kicker: { text: "AUJOURD'HUI" },
      headline: { text: "Bonjour" },
      subline: { text: "Le beau temps revient" },
    },
  };
  const sampleDoc = (over: Partial<Expression>): DashboardDoc =>
    thumbnailDoc(sampleModule, { x: 0, y: 1, w: 8, h: 3 }, {
      ...theme,
      expression: { ...expr, ...over },
    });

  const colourOptions: PickerOption<ColourUse>[] = COLOUR_USES.map((value) => ({
    value,
    label: COLOUR_USE_LABEL[value],
    hint: COLOUR_USE_NOTE[value],
    doc: sampleDoc({ colourUse: value }),
  }));
  const brushOptions: PickerOption<Brush>[] = BRUSHES.map((value) => ({
    value,
    label: BRUSH_LABEL[value],
    hint: BRUSH_NOTE[value],
    doc: sampleDoc({ brush: value }),
  }));
  const textureOptions: PickerOption<PixelTexture>[] = PIXEL_TEXTURES.map((value) => ({
    value,
    label: PIXEL_TEXTURE_LABEL[value],
    hint: PIXEL_TEXTURE_NOTE[value],
    doc: sampleDoc({ pixelTexture: value }),
  }));

  return (
    <Card title="Theme" id="theme">
      <div className="theme-section" data-testid="theme-expression">
        <h3 className="theme-section-head">Appearance</h3>
        <p className="theme-section-lede">
          The whole dashboard&rsquo;s look, in three choices. Pick by picture.
        </p>

        <VisualPicker
          legend="Colour"
          value={expr.colourUse}
          options={colourOptions}
          sources={sources}
          now={now}
          onChange={(value) => setExpression({ colourUse: value })}
          testId="expr-colour"
        />
        <VisualPicker
          legend="Brush"
          value={expr.brush}
          options={brushOptions}
          sources={sources}
          now={now}
          onChange={(value) => setExpression({ brush: value })}
          testId="expr-brush"
        />
        <VisualPicker
          legend="Texture"
          value={expr.pixelTexture}
          options={textureOptions}
          sources={sources}
          now={now}
          onChange={(value) => setExpression({ pixelTexture: value })}
          testId="expr-texture"
        />

        <label className="field theme-larger">
          <span>
            <input
              type="checkbox"
              checked={expr.largerText}
              onChange={(event) => setExpression({ largerText: event.target.checked })}
              data-testid="expr-larger-text"
            />{" "}
            Larger text
          </span>
          <span className="field-hint">
            Bumps every line up one size, for reading across a room.
          </span>
        </label>
      </div>

      <Fold summary="Advanced" testId="theme-advanced">
        <label className="field">
          <span>Content padding</span>
          <input
            type="range"
            min={MIN_CONTENT_PADDING}
            max={MAX_CONTENT_PADDING}
            step={1}
            value={theme.contentPadding}
            onChange={(event) =>
              setTheme({ contentPadding: Number(event.target.value) })
            }
            data-testid="theme-padding"
          />
          <span className="field-hint" data-testid="theme-padding-readout">
            {theme.contentPadding} px a side. Usable area {content.w} by{" "}
            {content.h}.
          </span>
        </label>

        <label className="field">
          <span>Dashboard font</span>
          <select
            value={theme.typography.family}
            onChange={(event) =>
              setTheme({
                typography: {
                  ...theme.typography,
                  family: event.target.value as FontFamilyId,
                },
              })
            }
            data-testid="theme-family"
          >
            {FONT_FAMILY_IDS.map((family) => (
              <option key={family} value={family}>
                {FONT_FAMILY_META[family]?.label ?? family}
              </option>
            ))}
          </select>
          <span className="field-hint">
            {FONT_FAMILY_META[theme.typography.family]?.note}
          </span>
        </label>

        <label className="field">
          <span>Dashboard weight</span>
          <select
            value={theme.typography.weight}
            onChange={(event) =>
              setTheme({
                typography: {
                  ...theme.typography,
                  weight: event.target.value as FontWeight,
                },
              })
            }
            data-testid="theme-weight"
          >
            {FONT_WEIGHTS.map((weight) => (
              <option key={weight} value={weight}>
                {WEIGHT_LABEL[weight] ?? weight}
              </option>
            ))}
          </select>
        </label>

        <Banner tone="info" testId="theme-inheritance">
          <span>
            {inheritance.inheriting} of {inheritance.total} roles follow this
            font. {inheritance.overridden} keep their own.
          </span>
        </Banner>

        <div className="card-actions">
          <Button
            onClick={() => setConfirmApply(true)}
            disabled={inheritance.overridden === 0}
            testId="theme-apply-all"
          >
            Apply to all text roles
          </Button>
        </div>
        <p className="field-hint">
          Overwrites every role&rsquo;s own font. One version, so it rolls back.
        </p>

        <h3>Palette</h3>
        <p className="field-hint">
          Turn an accent off and the renderer draws the nearest colour instead.
        </p>
        {STRUCTURAL_PIGMENTS.map((pigment) => (
          <label className="field" key={pigment}>
            <span>
              <input
                type="checkbox"
                checked
                disabled
                data-testid={`theme-palette-${pigment}`}
              />{" "}
              {pigment} — structural
            </span>
            <span className="field-hint">
              {pigment === "black"
                ? "The panel's default ink. It cannot be switched off."
                : "The paper every module clears to. It cannot be switched off."}
            </span>
          </label>
        ))}
        {OPTIONAL_PIGMENTS.map((pigment) => (
          <label className="field" key={pigment}>
            <span>
              <input
                type="checkbox"
                checked={theme.palette[pigment]}
                onChange={(event) =>
                  setTheme({
                    palette: { ...theme.palette, [pigment]: event.target.checked },
                  })
                }
                data-testid={`theme-palette-${pigment}`}
              />{" "}
              {pigment}
            </span>
          </label>
        ))}
        {explanations.length > 0 && (
          <Banner tone="attention" testId="theme-palette-fallback">
            <span>{explanations.join(" ")}</span>
          </Banner>
        )}
      </Fold>

      <ConfirmDialog
        open={confirmApply}
        title="Apply this font to every text role?"
        confirmLabel="Apply to all"
        body={
          <>
            <p>
              {inheritance.overridden} text{" "}
              {inheritance.overridden === 1 ? "role keeps" : "roles keep"} their
              own font today. Applying makes all {inheritance.total} follow the
              dashboard font, which is currently{" "}
              {FONT_FAMILY_META[theme.typography.family]?.label}. Fixed-pitch
              roles lose their fixed pitch, so columns of numbers may shift as
              the values change.
            </p>
            <p className="field-hint">This is what the panel would become:</p>
            <div className="theme-preview" data-testid="theme-apply-preview">
              <FramePreview
                doc={previewDoc}
                sources={sources}
                now={now}
                scale={1}
                testId="theme-preview-canvas"
              />
            </div>
          </>
        }
        onCancel={() => setConfirmApply(false)}
        onConfirm={() => {
          onChange(previewDoc);
          setConfirmApply(false);
        }}
      />
    </Card>
  );
}
