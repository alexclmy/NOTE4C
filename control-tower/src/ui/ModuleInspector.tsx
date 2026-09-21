"use client";

import type { ReactNode } from "react";
import { z } from "zod";
import { FRAME_EDGES, type ModuleFrame, type ModuleInstance } from "@/core/model";
import { moduleDefinition } from "@/core/render/modules";
import { FONT_FAMILY_META } from "@/core/render/fonts";
import { FONT_FAMILY_IDS, FONT_SIZES, FONT_WEIGHTS } from "@/core/font";
import { supportedPictograms } from "@/core/render/pictograms";
import { PREVIEW_RGB } from "@/core/palette";
import {
  COLOUR_TOKENS,
  COLOUR_TOKEN_LABEL,
  DEFAULT_THEME,
  TOKEN_PIGMENT,
  type ColourToken,
  type DashboardTheme,
  type StyleFamily,
  type StyleWeight,
} from "@/core/theme";
import {
  TEXT_ALIGNMENTS,
  TEXT_ELEMENT_TAG,
  TEXT_STYLE_TAG,
  type ContrastFact,
  type LayoutNote,
  type TextElement,
  type TextStyle,
} from "@/core/render/text";
import {
  LIST_ROWS_TAG,
  type ListRow,
} from "@/core/render/modules/list";
import { ADVANCED_OPTION_TAG, LAYOUT_VARIANT_TAG, cellsToPixels } from "@/core/render/types";
import { IMAGE_SOURCE_TAG, type ImageSource } from "@/core/render/modules/image";
import { ImageField } from "./ImageField";
import type { DashboardSources } from "@/core/render/data";
import { VisualPicker, thumbnailDoc, type PickerOption } from "./VisualPicker";
import { Fold } from "./Disclosure";
import {
  CONDITIONS_TAG,
  CONDITION_KINDS,
  CONDITION_SOURCES,
  DAY_LABELS,
  SOURCE_STATES,
  explainCondition,
  type Condition,
  type ConditionKind,
} from "@/core/render/modules/conditionalMessage";
import { PANEL_TIMEZONE } from "@/core/render/time";

/**
 * Schema-driven inspector.
 *
 * The form is generated from each module's own zod schema, so a module can
 * never grow an option the designer cannot set, and the designer can never
 * offer an option the renderer will reject.
 *
 * Text elements, text styles, list rows and conditions get real widgets rather
 * than a generic object editor, and they are recognised by the tag each schema
 * carries in its own `.describe()` rather than by a list of field names kept in
 * this file. A new text role on a module shows up here with no change to this
 * component.
 */

type FieldKind =
  | { kind: "text-element" }
  | { kind: "text-style" }
  | { kind: "list-rows" }
  | { kind: "conditions" }
  | { kind: "colour-token" }
  | { kind: "layout-variant"; values: string[] }
  | { kind: "string"; max?: number }
  | { kind: "boolean" }
  | { kind: "number"; min?: number; max?: number }
  | { kind: "enum"; values: string[] }
  | { kind: "datetime-nullable" }
  | { kind: "image-source" }
  | { kind: "unsupported" };

function unwrap(schema: z.ZodTypeAny): z.ZodTypeAny {
  let current = schema;
  for (let i = 0; i < 8; i += 1) {
    if (current instanceof z.ZodDefault) {
      current = current._def.innerType as z.ZodTypeAny;
      continue;
    }
    if (current instanceof z.ZodOptional) {
      current = current._def.innerType as z.ZodTypeAny;
      continue;
    }
    if (current instanceof z.ZodEffects) {
      current = current._def.schema as z.ZodTypeAny;
      continue;
    }
    break;
  }
  return current;
}

/**
 * The tag survives .default(), which wraps the object rather than replacing
 * it, so look at both the wrapper and what it wraps.
 */
function taggedAs(schema: z.ZodTypeAny, tag: string): boolean {
  return schema.description === tag || unwrap(schema).description === tag;
}

/** A field the module tagged to live behind the "Advanced" fold. */
function isAdvanced(schema: z.ZodTypeAny): boolean {
  return taggedAs(schema, ADVANCED_OPTION_TAG);
}

function describe(schema: z.ZodTypeAny): FieldKind {
  if (taggedAs(schema, IMAGE_SOURCE_TAG)) return { kind: "image-source" };
  if (taggedAs(schema, TEXT_ELEMENT_TAG)) return { kind: "text-element" };
  if (taggedAs(schema, TEXT_STYLE_TAG)) return { kind: "text-style" };
  if (taggedAs(schema, LIST_ROWS_TAG)) return { kind: "list-rows" };
  if (taggedAs(schema, CONDITIONS_TAG)) return { kind: "conditions" };

  const inner = unwrap(schema);

  if (taggedAs(schema, LAYOUT_VARIANT_TAG) && inner instanceof z.ZodEnum) {
    return { kind: "layout-variant", values: [...(inner._def.values as string[])] };
  }

  if (inner instanceof z.ZodNullable) {
    const wrapped = unwrap(inner._def.innerType as z.ZodTypeAny);
    if (wrapped instanceof z.ZodString) return { kind: "datetime-nullable" };
  }
  if (inner instanceof z.ZodBoolean) return { kind: "boolean" };
  if (inner instanceof z.ZodEnum) {
    const values = [...(inner._def.values as string[])];
    // A colour token is an enum, but it deserves pigment names rather than the
    // raw words, so it gets its own widget.
    const isColour =
      values.length === COLOUR_TOKENS.length &&
      values.every((value) => (COLOUR_TOKENS as readonly string[]).includes(value));
    return isColour ? { kind: "colour-token" } : { kind: "enum", values };
  }
  if (inner instanceof z.ZodNumber) {
    const checks = inner._def.checks ?? [];
    const min = checks.find((c) => c.kind === "min");
    const max = checks.find((c) => c.kind === "max");
    return {
      kind: "number",
      ...(min && "value" in min ? { min: min.value as number } : {}),
      ...(max && "value" in max ? { max: max.value as number } : {}),
    };
  }
  if (inner instanceof z.ZodString) {
    const checks = inner._def.checks ?? [];
    const max = checks.find((c) => c.kind === "max");
    return { kind: "string", ...(max && "value" in max ? { max: max.value as number } : {}) };
  }
  return { kind: "unsupported" };
}

/** Longest string this text role accepts, read off its own schema. */
function maxLengthOf(schema: z.ZodTypeAny): number | undefined {
  const inner = unwrap(schema);
  if (!(inner instanceof z.ZodObject)) return undefined;
  const textField = unwrap(inner.shape.text as z.ZodTypeAny);
  if (!(textField instanceof z.ZodString)) return undefined;
  const max = (textField._def.checks ?? []).find((c) => c.kind === "max");
  return max && "value" in max ? (max.value as number) : undefined;
}

function humanise(key: string): string {
  const spaced = key.replace(/([A-Z])/g, " $1").toLowerCase().trim();
  return spaced.charAt(0).toUpperCase() + spaced.slice(1);
}

/**
 * What to call a style's family in a summary line.
 *
 * "inherit" is a real stored value, so it needs a name of its own that says
 * where the face is actually coming from. A role that inherits and a role that
 * happens to be set to today's dashboard font must not read the same.
 */
export function familyLabel(
  family: StyleFamily,
  theme: DashboardTheme = DEFAULT_THEME,
): string {
  if (family === "inherit") {
    return `Dashboard font (${FONT_FAMILY_META[theme.typography.family]?.label ?? theme.typography.family})`;
  }
  return FONT_FAMILY_META[family]?.label ?? family;
}

/** The family's own description, for the select's tooltip. */
function familyNote(family: StyleFamily): string | undefined {
  if (family === "inherit") {
    return "Follows the dashboard font. Change it once in Theme and every inheriting role follows.";
  }
  return FONT_FAMILY_META[family]?.note;
}

const ALIGN_LABEL: Record<string, string> = {
  left: "Left",
  center: "Centre",
  right: "Right",
};

const WEIGHT_LABEL: Record<string, string> = {
  regular: "Regular",
  bold: "Bold",
  inherit: "Dashboard weight",
};

/** Friendlier words for the enums a module offers. Falls back to the value. */
const ENUM_LABEL: Record<string, string> = {
  bullet: "Bullet",
  numbered: "Numbered",
  checkbox: "Checkbox (drawing only)",
  auto: "Automatic",
  days: "Days",
  hours: "Hours",
  minutes: "Minutes",
  showPassed: "Show the passed message",
  hide: "Draw nothing",
  showFallback: "Show the fallback text",
  all: "All of them",
  any: "Any of them",
};

/**
 * Text roles a module draws only when something is off — data missing, a
 * reading gone stale, a list empty, a countdown already passed. They are the
 * owner's words to change, but they are not what anyone opens a module to
 * write, so they live in the Fine-tune fold rather than on the first surface.
 * Recognised by key name because they share no schema tag with the primary
 * wording — a role is one of these because of what it means, not how it is
 * typed.
 */
const FALLBACK_ROLES = new Set([
  "unavailableTitle",
  "unavailableNote",
  "unavailableText",
  "staleLabel",
  "staleSuffix",
  "emptyTitle",
  "emptyNote",
  "emptyText",
  "moreText",
  "passedText",
  "invalidExpiryText",
]);

/** The {placeholders} this role's default wording offers. */
function placeholdersFor(defaultText: unknown): string[] {
  if (typeof defaultText !== "string") return [];
  return [...defaultText.matchAll(/\{(\w+)\}/g)].map((match) => match[1] as string);
}

/**
 * The five colour tokens, as swatches of the pigment each one resolves to.
 *
 * Five, not the panel's four, and the difference is the whole model. What is
 * stored is a *token* — inherit, ink, paper, accent, highlight — and the theme
 * decides which pigment it prints as, which is what lets a composition switch a
 * pigment off and have every element follow. A four-swatch picker of raw
 * pigments would be storing the answer instead of the question.
 *
 * So the swatch shows the resolved pigment and the label names the token: what
 * is on the button is what lands on the panel, and what is saved is the thing
 * that survives a theme change. "Inherit" has no pigment of its own to show —
 * it is whatever the module chose for this element — so it is drawn with a
 * diagonal rather than a colour, because a blank white square would read as
 * paper, which is a pigment.
 *
 * A row of buttons rather than a `<select>`: these are five fixed choices whose
 * whole meaning is visual, and a dropdown that has to be opened to see the
 * colours is a dropdown that hides its own options. 44 px, because the browser
 * QA measures every target on this page.
 */
function ColourSelect({
  id,
  value,
  onChange,
  label,
}: {
  id: string;
  value: ColourToken;
  onChange: (next: ColourToken) => void;
  label: string;
}) {
  return (
    <div className="type-field" data-testid={id}>
      <span id={`${id}-label`}>{label}</span>
      <div
        className="ink-chips"
        role="group"
        aria-labelledby={`${id}-label`}
        title="The panel has four pigments; there is no fifth colour to pick."
      >
        {COLOUR_TOKENS.map((token) => (
          <button
            type="button"
            key={token}
            className="ink-chip"
            data-token={token}
            aria-pressed={value === token}
            aria-label={COLOUR_TOKEN_LABEL[token]}
            title={COLOUR_TOKEN_LABEL[token]}
            onClick={() => onChange(token)}
            data-testid={`${id}-${token}`}
            style={
              token === "inherit"
                ? undefined
                : {
                    background:
                      PREVIEW_RGB[TOKEN_PIGMENT[token]] ?? PREVIEW_RGB[1],
                  }
            }
          />
        ))}
      </div>
    </div>
  );
}

function TypographyControls({
  id,
  style,
  theme,
  notes,
  onChange,
}: {
  id: string;
  style: TextStyle;
  theme: DashboardTheme;
  /** Layout notes for this role, from the live render. */
  notes: LayoutNote[];
  onChange: (next: TextStyle) => void;
}) {
  const set = <K extends keyof TextStyle>(key: K, value: TextStyle[K]): void => {
    onChange({ ...style, [key]: value });
  };
  const inertGap = notes.find((note) => note.kind === "line-gap-inert");

  return (
    <div className="type-grid">
      <label className="type-field">
        <span>Font</span>
        <select
          value={style.family}
          onChange={(event) => set("family", event.target.value as StyleFamily)}
          data-testid={`${id}-family`}
          title={familyNote(style.family)}
        >
          {/*
            Inheritance first, and named for the face it currently resolves to,
            so the difference between following the dashboard and happening to
            match it is visible without opening anything.
          */}
          <option value="inherit">{familyLabel("inherit", theme)}</option>
          {/*
            FONT_FAMILY_IDS, not the manifest's keys: the manifest is written
            sorted, and this list should be in the order the families were
            chosen, with the default first.
          */}
          {FONT_FAMILY_IDS.map((family) => (
            <option key={family} value={family}>
              {FONT_FAMILY_META[family]?.label ?? family}
            </option>
          ))}
        </select>
      </label>

      <label className="type-field">
        <span>Size</span>
        <select
          value={style.size}
          onChange={(event) =>
            set("size", Number(event.target.value) as TextStyle["size"])
          }
          data-testid={`${id}-size`}
        >
          {FONT_SIZES.map((size) => (
            <option key={size} value={size}>
              {size} px
            </option>
          ))}
        </select>
      </label>

      <label className="type-field">
        <span>Weight</span>
        <select
          value={style.weight}
          onChange={(event) => set("weight", event.target.value as StyleWeight)}
          data-testid={`${id}-weight`}
        >
          <option value="inherit">
            Dashboard weight ({WEIGHT_LABEL[theme.typography.weight]})
          </option>
          {FONT_WEIGHTS.map((weight) => (
            <option key={weight} value={weight}>
              {WEIGHT_LABEL[weight] ?? weight}
            </option>
          ))}
        </select>
      </label>

      <label className="type-field">
        <span>Align</span>
        <select
          value={style.align}
          onChange={(event) =>
            set("align", event.target.value as TextStyle["align"])
          }
          data-testid={`${id}-align`}
        >
          {TEXT_ALIGNMENTS.map((align) => (
            <option key={align} value={align}>
              {ALIGN_LABEL[align] ?? align}
            </option>
          ))}
        </select>
      </label>

      <label className="type-field">
        <span>Line gap</span>
        <select
          value={style.lineSpacing}
          onChange={(event) => set("lineSpacing", Number(event.target.value))}
          data-testid={`${id}-lineSpacing`}
        >
          {[0, 1, 2, 3, 4, 6, 8, 12].map((gap) => (
            <option key={gap} value={gap}>
              {gap} px
            </option>
          ))}
        </select>
      </label>

      <ColourSelect
        id={`${id}-colour`}
        label="Colour"
        value={style.colour}
        onChange={(colour) => set("colour", colour)}
      />

      {inertGap && (
        /*
          The honest answer to "the line gap does nothing". It really does
          nothing HERE, because there is no second line for it to move, and
          saying which is better than making the control feel broken.
        */
        <p className="field-hint" data-testid={`${id}-lineSpacing-inert`}>
          {inertGap.detail}
        </p>
      )}
    </div>
  );
}

/** The panel's whole symbol vocabulary, so the limits are where the words are. */
function SymbolHelp({ id }: { id: string }) {
  const groups = supportedPictograms();
  const total = groups.reduce((sum, group) => sum + group.entries.length, 0);
  return (
    <details className="type-details">
      <summary data-testid={`${id}-symbols`}>
        Symbols the panel can draw ({total})
      </summary>
      <p className="field-hint">
        Drawn by this project in the panel&rsquo;s four colours, not an emoji
        font. Anything outside this set becomes a boxed question mark. Skin
        tones and variation selectors are ignored; joined sequences resolve to
        their parts.
      </p>
      {groups.map((group) => (
        <p className="field-hint" key={group.category}>
          <strong>{group.category}</strong>{" "}
          {group.entries.map((entry) => entry.emoji).join(" ")}
        </p>
      ))}
    </details>
  );
}

/**
 * A text role, split into its two jobs.
 *
 * `part="words"` is the thing a person came to write: the wording, whether it
 * shows, and the notes that belong beside the words (an unreadable colour, an
 * emoji the panel cannot draw). It is what the default surface shows.
 *
 * `part="type"` is how the words are set — face, size, weight, alignment, line
 * gap, colour. It used to sit behind a per-role "Type" disclosure on every
 * role; now it lives once, in the module's single "Fine-tune" fold, so the
 * first surface is words and a picture and nothing else.
 */
function TextElementField({
  fieldKey,
  label,
  value,
  maxLength,
  placeholders,
  theme,
  notes,
  contrasts,
  onChange,
  part = "words",
}: {
  fieldKey: string;
  label: string;
  value: TextElement;
  maxLength?: number;
  placeholders: string[];
  theme: DashboardTheme;
  notes: LayoutNote[];
  contrasts: ContrastFact[];
  onChange: (next: TextElement) => void;
  part?: "words" | "type";
}) {
  const id = `opt-${fieldKey}`;
  const empty = value.text.trim().length === 0;
  const missingSymbols = notes.find((note) => note.kind === "unsupported-symbols");
  const contrast = contrasts[0];

  if (part === "type") {
    return (
      <div className="finetune-role">
        <span className="finetune-role-label">{label}</span>
        <TypographyControls
          id={id}
          style={value.style}
          theme={theme}
          notes={notes}
          onChange={(style) => onChange({ ...value, style })}
        />
        <SymbolHelp id={id} />
      </div>
    );
  }

  return (
    <div className="text-role" data-testid={`${id}-role`}>
      <div className="text-role-head">
        <label htmlFor={id}>{label}</label>
        <label className="text-role-toggle">
          <input
            type="checkbox"
            checked={value.visible}
            onChange={(event) =>
              onChange({ ...value, visible: event.target.checked })
            }
            data-testid={`${id}-visible`}
          />{" "}
          Shown
        </label>
      </div>

      <input
        id={id}
        type="text"
        value={value.text}
        maxLength={maxLength}
        placeholder="Leave empty to draw nothing"
        onChange={(event) => onChange({ ...value, text: event.target.value })}
        data-testid={id}
      />

      {placeholders.length > 0 && (
        <p className="field-hint">
          {placeholders.map((name) => `{${name}}`).join(" ")} come from live data.
        </p>
      )}
      {value.visible && empty && (
        <p className="field-hint">Empty, so nothing is drawn here.</p>
      )}
      {missingSymbols && (
        <p className="overflow-note" data-testid={`${id}-symbols-note`}>
          {missingSymbols.detail}
        </p>
      )}
      {contrast && (
        <p className="overflow-note" data-testid={`${id}-contrast`}>
          {contrast.verdict === "invisible"
            ? `${contrast.foreground} on ${contrast.background} cannot be read — it is drawn, then invisible.`
            : `${contrast.foreground} on ${contrast.background} is very pale on the panel.`}
        </p>
      )}
    </div>
  );
}

/**
 * The list rows editor.
 *
 * Reordering moves a row rather than swapping its text, so a row keeps its own
 * visibility as it travels. Hiding is separate from deleting for the same
 * reason it is on a module: "not this week" should not cost you the wording.
 */
function ListRowsField({
  rows,
  onChange,
}: {
  rows: ListRow[];
  onChange: (next: ListRow[]) => void;
}) {
  const move = (index: number, delta: number): void => {
    const target = index + delta;
    if (target < 0 || target >= rows.length) return;
    const next = [...rows];
    const [moved] = next.splice(index, 1);
    if (moved) next.splice(target, 0, moved);
    onChange(next);
  };

  return (
    <div className="text-role" data-testid="opt-rows-role">
      <div className="text-role-head">
        <label>Rows</label>
        <span className="field-hint">{rows.length} of 12</span>
      </div>

      {rows.length === 0 && (
        <p className="field-hint" data-testid="opt-rows-empty">
          No rows yet — the panel draws its empty state until you add one.
        </p>
      )}

      <ul className="row-editor">
        {rows.map((row, index) => (
          <li key={index} data-testid={`opt-row-${index}`}>
            <input
              type="text"
              value={row.text}
              maxLength={60}
              placeholder="What goes on this line"
              onChange={(event) => {
                const next = [...rows];
                next[index] = { ...row, text: event.target.value };
                onChange(next);
              }}
              data-testid={`opt-row-${index}-text`}
            />
            <label className="text-role-toggle">
              <input
                type="checkbox"
                checked={row.visible}
                onChange={(event) => {
                  const next = [...rows];
                  next[index] = { ...row, visible: event.target.checked };
                  onChange(next);
                }}
                data-testid={`opt-row-${index}-visible`}
              />{" "}
              Shown
            </label>
            {/*
              Explicit names, because the visible label of each of these is an
              arrow or the word "Delete" and there are as many sets of them as
              there are rows. Announced from the content alone they are "up
              arrow", "down arrow" and "Delete" — repeated N times, with nothing
              to say which row is about to move or go. The `title` gave a
              sighted mouse user "Move up"; the row number was nobody's.
            */}
            <button
              type="button"
              className="btn btn-quiet"
              onClick={() => move(index, -1)}
              disabled={index === 0}
              aria-label={`Move row ${index + 1} up`}
              title="Move up"
              data-testid={`opt-row-${index}-up`}
            >
              ↑
            </button>
            <button
              type="button"
              className="btn btn-quiet"
              onClick={() => move(index, 1)}
              disabled={index === rows.length - 1}
              aria-label={`Move row ${index + 1} down`}
              title="Move down"
              data-testid={`opt-row-${index}-down`}
            >
              ↓
            </button>
            <button
              type="button"
              className="btn btn-danger"
              onClick={() => onChange(rows.filter((_, i) => i !== index))}
              aria-label={`Delete row ${index + 1} and its wording`}
              title="Delete this row and its wording"
              data-testid={`opt-row-${index}-remove`}
            >
              Delete
            </button>
          </li>
        ))}
      </ul>

      <button
        type="button"
        className="btn btn-default"
        onClick={() => onChange([...rows, { text: "", visible: true }])}
        disabled={rows.length >= 12}
        data-testid="opt-rows-add"
      >
        Add row
      </button>
    </div>
  );
}

const CONDITION_KIND_LABEL: Record<ConditionKind, string> = {
  dateRange: "Between two dates",
  daysOfWeek: "On certain days of the week",
  sourceState: "When a source is in a state",
  remindersCount: "By how many reminders are open",
};

function blankCondition(kind: ConditionKind): Condition {
  switch (kind) {
    case "dateRange":
      return { kind, from: null, to: null, timeZone: PANEL_TIMEZONE };
    case "daysOfWeek":
      return { kind, days: [], timeZone: PANEL_TIMEZONE };
    case "sourceState":
      return { kind, source: "calendar", state: "ok" };
    case "remindersCount":
    default:
      return { kind: "remindersCount", compare: "atLeast", value: 1 };
  }
}

/**
 * The conditions editor.
 *
 * Every condition is a typed object with its own small form, and every one of
 * them prints the sentence it means underneath. There is deliberately no place
 * to type an expression: the set of things this can decide is the set of forms
 * below, and it is meant to stay small enough to explain.
 */
function ConditionsField({
  conditions,
  onChange,
}: {
  conditions: Condition[];
  onChange: (next: Condition[]) => void;
}) {
  const update = (index: number, next: Condition): void => {
    const copy = [...conditions];
    copy[index] = next;
    onChange(copy);
  };

  return (
    <div className="text-role" data-testid="opt-conditions-role">
      <div className="text-role-head">
        <label>Conditions</label>
        <span className="field-hint">{conditions.length} of 4</span>
      </div>

      {conditions.length === 0 && (
        <p className="field-hint" data-testid="opt-conditions-empty">
          No condition yet, so the panel draws a setup state. Without one this
          would just be a message.
        </p>
      )}

      <ul className="row-editor">
        {conditions.map((condition, index) => (
          <li key={index} className="condition" data-testid={`opt-condition-${index}`}>
            <select
              value={condition.kind}
              onChange={(event) =>
                update(index, blankCondition(event.target.value as ConditionKind))
              }
              data-testid={`opt-condition-${index}-kind`}
            >
              {CONDITION_KINDS.map((kind) => (
                <option key={kind} value={kind}>
                  {CONDITION_KIND_LABEL[kind]}
                </option>
              ))}
            </select>

            {condition.kind === "dateRange" && (
              <span className="condition-fields">
                <input
                  type="date"
                  value={condition.from ?? ""}
                  onChange={(event) =>
                    update(index, {
                      ...condition,
                      from: event.target.value === "" ? null : event.target.value,
                    })
                  }
                  data-testid={`opt-condition-${index}-from`}
                />
                <input
                  type="date"
                  value={condition.to ?? ""}
                  onChange={(event) =>
                    update(index, {
                      ...condition,
                      to: event.target.value === "" ? null : event.target.value,
                    })
                  }
                  data-testid={`opt-condition-${index}-to`}
                />
              </span>
            )}

            {condition.kind === "daysOfWeek" && (
              <span className="condition-fields">
                {DAY_LABELS.map((day, dayIndex) => (
                  <label key={day} className="text-role-toggle">
                    <input
                      type="checkbox"
                      checked={condition.days.includes(dayIndex)}
                      onChange={(event) =>
                        update(index, {
                          ...condition,
                          days: event.target.checked
                            ? [...condition.days, dayIndex].sort((a, b) => a - b)
                            : condition.days.filter((d) => d !== dayIndex),
                        })
                      }
                      data-testid={`opt-condition-${index}-day-${dayIndex}`}
                    />{" "}
                    {day.slice(0, 3)}
                  </label>
                ))}
              </span>
            )}

            {condition.kind === "sourceState" && (
              <span className="condition-fields">
                <select
                  value={condition.source}
                  onChange={(event) =>
                    update(index, {
                      ...condition,
                      source: event.target.value as (typeof CONDITION_SOURCES)[number],
                    })
                  }
                  data-testid={`opt-condition-${index}-source`}
                >
                  {CONDITION_SOURCES.map((source) => (
                    <option key={source} value={source}>
                      {humanise(source)}
                    </option>
                  ))}
                </select>
                <select
                  value={condition.state}
                  onChange={(event) =>
                    update(index, {
                      ...condition,
                      state: event.target.value as (typeof SOURCE_STATES)[number],
                    })
                  }
                  data-testid={`opt-condition-${index}-state`}
                >
                  {SOURCE_STATES.map((state) => (
                    <option key={state} value={state}>
                      {state}
                    </option>
                  ))}
                </select>
              </span>
            )}

            {condition.kind === "remindersCount" && (
              <span className="condition-fields">
                <select
                  value={condition.compare}
                  onChange={(event) =>
                    update(index, {
                      ...condition,
                      compare: event.target.value as "atLeast" | "atMost",
                    })
                  }
                  data-testid={`opt-condition-${index}-compare`}
                >
                  <option value="atLeast">At least</option>
                  <option value="atMost">At most</option>
                </select>
                <input
                  type="number"
                  min={0}
                  max={99}
                  value={condition.value}
                  onChange={(event) =>
                    update(index, {
                      ...condition,
                      value: Number(event.target.value),
                    })
                  }
                  data-testid={`opt-condition-${index}-value`}
                />
              </span>
            )}

            <button
              type="button"
              className="btn btn-danger"
              onClick={() => onChange(conditions.filter((_, i) => i !== index))}
              data-testid={`opt-condition-${index}-remove`}
            >
              Delete
            </button>

            <p className="field-hint" data-testid={`opt-condition-${index}-explain`}>
              {explainCondition(condition)}
            </p>
          </li>
        ))}
      </ul>

      <button
        type="button"
        className="btn btn-default"
        onClick={() => onChange([...conditions, blankCondition("daysOfWeek")])}
        disabled={conditions.length >= 4}
        data-testid="opt-conditions-add"
      >
        Add condition
      </button>
      <p className="field-hint">
        This is not an alarm. A condition decides what the next push draws.
      </p>
    </div>
  );
}

type FrameEdge = ModuleFrame["edges"][number];

const FRAME_EDGE_LABEL: Record<FrameEdge, string> = {
  top: "Top",
  right: "Right",
  bottom: "Bottom",
  left: "Left",
};

const FRAME_STYLE_LABEL: Record<ModuleFrame["style"], string> = {
  solid: "Solid",
  dashed: "Dashed",
  dotted: "Dotted",
};

/**
 * Frame (divider) controls.
 *
 * A frame is chrome that lives on the instance, not an option in the module's
 * schema, so it gets its own widget and its own update channel rather than
 * riding the schema-driven form. Toggling every edge off removes the frame
 * entirely (undefined), so a module with no rules stores nothing.
 */
function FrameControls({
  frame,
  onChange,
}: {
  frame: ModuleFrame | undefined;
  onChange: (next: ModuleFrame | undefined) => void;
}) {
  const edges = frame?.edges ?? [];
  const weight = frame?.weight ?? 2;
  const style = frame?.style ?? "solid";
  const inset = frame?.inset ?? 0;
  const color = frame?.color ?? 0;

  function emit(nextEdges: FrameEdge[], part: Partial<ModuleFrame> = {}): void {
    if (nextEdges.length === 0) {
      onChange(undefined);
      return;
    }
    onChange({ edges: nextEdges, weight, style, inset, color, ...part });
  }

  function toggle(edge: FrameEdge): void {
    emit(edges.includes(edge) ? edges.filter((e) => e !== edge) : [...edges, edge]);
  }

  const active = edges.length > 0;

  return (
    <div data-testid="frame-controls">
      <p className="field-hint">
        Hairline rules that delimit this module from what abuts it.
      </p>
      <div className="type-field" data-testid="frame-edges">
        <span id="frame-edges-label">Rules on</span>
        <div className="frame-edges" role="group" aria-labelledby="frame-edges-label">
          {FRAME_EDGES.map((edge) => (
            <button
              type="button"
              key={edge}
              className="frame-edge"
              aria-pressed={edges.includes(edge)}
              aria-label={FRAME_EDGE_LABEL[edge]}
              data-testid={`frame-edge-${edge}`}
              onClick={() => toggle(edge)}
            >
              {FRAME_EDGE_LABEL[edge]}
            </button>
          ))}
        </div>
      </div>

      <div className="type-grid">
        <label className="type-field">
          <span>Weight</span>
          <select
            value={weight}
            disabled={!active}
            data-testid="frame-weight"
            onChange={(event) => emit(edges, { weight: Number(event.target.value) })}
          >
            {[1, 2, 3, 4].map((w) => (
              <option key={w} value={w}>
                {w} px
              </option>
            ))}
          </select>
        </label>
        <label className="type-field">
          <span>Style</span>
          <select
            value={style}
            disabled={!active}
            data-testid="frame-style"
            onChange={(event) =>
              emit(edges, { style: event.target.value as ModuleFrame["style"] })
            }
          >
            {(Object.keys(FRAME_STYLE_LABEL) as ModuleFrame["style"][]).map((s) => (
              <option key={s} value={s}>
                {FRAME_STYLE_LABEL[s]}
              </option>
            ))}
          </select>
        </label>
      </div>
    </div>
  );
}

export function ModuleInspector({
  module,
  onChange,
  onFrameChange,
  problems,
  overflows = [],
  notes = [],
  contrasts = [],
  theme = DEFAULT_THEME,
  sources,
  now,
}: {
  module: ModuleInstance;
  onChange: (options: Record<string, unknown>) => void;
  /** Set the module's frame (dividers). Omit to hide the Separators section. */
  onFrameChange?: (frame: ModuleFrame | undefined) => void;
  problems: string[];
  /** Roles whose text did not fit, from the live render of this dashboard. */
  overflows?: Array<{ role: string; kind: "width" | "height" }>;
  /** Layout observations for this module, from the same render. */
  notes?: LayoutNote[];
  /** Colour combinations this module cannot show, from the same render. */
  contrasts?: ContrastFact[];
  theme?: DashboardTheme;
  /** Live sources and clock, so the disposition thumbnails are real renders. */
  sources?: DashboardSources;
  now?: Date;
}) {
  const definition = moduleDefinition(module.type);
  const shape =
    definition.schema instanceof z.ZodObject
      ? (definition.schema.shape as Record<string, z.ZodTypeAny>)
      : {};

  const options = module.options as Record<string, unknown>;
  const defaults = definition.defaultOptions as Record<string, unknown>;

  function set(key: string, value: unknown): void {
    onChange({ ...options, [key]: value });
  }

  const entries = Object.entries(shape);
  const variantEntry = entries.find(
    ([, schema]) => describe(schema).kind === "layout-variant",
  );
  const bodyEntries = entries.filter(([key]) => key !== variantEntry?.[0]);

  const renderField = ([key, fieldSchema]: [string, z.ZodTypeAny]) => {
        const field = describe(fieldSchema);
        const value = options[key];
        const id = `opt-${key}`;
        const overflowed = overflows.filter((entry) => entry.role === key);
        const roleNotes = notes.filter((entry) => entry.role === key);
        const roleContrasts = contrasts.filter((entry) => entry.role === key);

        if (field.kind === "image-source") {
          const source = value as ImageSource | undefined;
          if (!source || typeof source !== "object") return null;
          const tile = cellsToPixels(
            module.x,
            module.y,
            module.w,
            module.h,
            theme?.contentPadding ?? 0,
          );
          return (
            <div key={key}>
              <ImageField
                value={source}
                tileW={tile.w}
                tileH={tile.h}
                onChange={(next) => set(key, next)}
              />
            </div>
          );
        }

        if (field.kind === "text-style") {
          const style = value as TextStyle | undefined;
          if (!style || typeof style !== "object") return null;
          // No wording of its own — the value is the source's; only how it is
          // set is the owner's. Rendered flat inside the module's Fine-tune
          // fold, so it needs no disclosure of its own.
          return (
            <div className="finetune-role" key={key}>
              <span className="finetune-role-label">
                {humanise(key).replace(/ style$/, "")}
              </span>
              <p className="field-hint">
                This value comes from the source; only how it is set is yours.
              </p>
              <TypographyControls
                id={id}
                style={style}
                theme={theme}
                notes={roleNotes}
                onChange={(next) => set(key, next)}
              />
              {overflowed.length > 0 && (
                <p className="overflow-note" data-testid={`${id}-overflow`}>
                  Does not fit its tile at this size.
                </p>
              )}
            </div>
          );
        }

        if (field.kind === "list-rows") {
          const rows = Array.isArray(value) ? (value as ListRow[]) : [];
          return (
            <div key={key}>
              <ListRowsField rows={rows} onChange={(next) => set(key, next)} />
              {overflowed.length > 0 && (
                <p className="overflow-note" data-testid={`${id}-overflow`}>
                  More rows than fit — the extra ones are counted on the panel,
                  not dropped.
                </p>
              )}
            </div>
          );
        }

        if (field.kind === "conditions") {
          const conditions = Array.isArray(value) ? (value as Condition[]) : [];
          return (
            <ConditionsField
              key={key}
              conditions={conditions}
              onChange={(next) => set(key, next)}
            />
          );
        }

        if (field.kind === "colour-token") {
          return (
            <div className="type-grid" key={key}>
              <ColourSelect
                id={id}
                label={humanise(key)}
                value={(value as ColourToken) ?? "inherit"}
                onChange={(token) => set(key, token)}
              />
            </div>
          );
        }

        if (field.kind === "boolean") {
          return (
            <label className="field" key={key}>
              <span>
                <input
                  id={id}
                  type="checkbox"
                  checked={value === true}
                  onChange={(event) => set(key, event.target.checked)}
                  data-testid={id}
                />{" "}
                {humanise(key)}
              </span>
              {key === "showProvenance" && (
                <span className="field-hint">
                  Print where the data came from. Off by default.
                </span>
              )}
            </label>
          );
        }

        if (field.kind === "enum") {
          return (
            <label className="field" key={key}>
              <span>{humanise(key)}</span>
              <select
                id={id}
                value={String(value ?? "")}
                onChange={(event) => set(key, event.target.value)}
                data-testid={id}
              >
                {field.values.map((option) => (
                  <option key={option} value={option}>
                    {ENUM_LABEL[option] ?? option}
                  </option>
                ))}
              </select>
              {key === "marker" && (
                <span className="field-hint">
                  A checkbox is a drawing. The panel has no buttons, so nothing
                  on it can be ticked.
                </span>
              )}
              {key === "unit" && (
                <span className="field-hint">
                  Counted between calendar dates: three days is three sleeps.
                </span>
              )}
            </label>
          );
        }

        if (field.kind === "number") {
          return (
            <label className="field" key={key}>
              <span>{humanise(key)}</span>
              <input
                id={id}
                type="number"
                value={typeof value === "number" ? value : ""}
                min={field.min}
                max={field.max}
                onChange={(event) => set(key, Number(event.target.value))}
                data-testid={id}
              />
              {key === "maxVisibleRows" && (
                <span className="field-hint">
                  How many rows may be drawn. Extra rows are counted, never dropped.
                </span>
              )}
            </label>
          );
        }

        if (field.kind === "datetime-nullable") {
          const current =
            typeof value === "string" && value.length > 0
              ? new Date(value).toISOString().slice(0, 16)
              : "";
          return (
            <label className="field" key={key}>
              <span>{humanise(key)}</span>
              <input
                id={id}
                type="datetime-local"
                value={current}
                onChange={(event) =>
                  set(
                    key,
                    event.target.value.length === 0
                      ? null
                      : new Date(event.target.value).toISOString(),
                  )
                }
                data-testid={id}
              />
              <span className="field-hint">
                {key === "targetAt"
                  ? "Empty means no target — the tile says so instead of counting to zero."
                  : "Leave empty for no expiry."}
              </span>
            </label>
          );
        }

        if (field.kind === "string") {
          return (
            <label className="field" key={key}>
              <span>{humanise(key)}</span>
              <input
                id={id}
                type="text"
                value={typeof value === "string" ? value : ""}
                maxLength={field.max}
                onChange={(event) => set(key, event.target.value)}
                data-testid={id}
                {...(key === "timeZone" ? { list: "known-timezones" } : {})}
              />
              {key === "timeZone" && (
                <>
                  {/*
                    Suggestions, not a supported list — the field accepts any
                    IANA name this runtime knows. The machine's own zone is
                    offered first, because a panel on a wall is almost always
                    in it; the rest are spread across the world rather than
                    clustered near whoever wrote this, which is what the list
                    used to be.
                  */}
                  <datalist id="known-timezones">
                    {[
                      ...new Set([
                        PANEL_TIMEZONE,
                        "UTC",
                        "Europe/London",
                        "Europe/Paris",
                        "America/New_York",
                        "America/Los_Angeles",
                        "Asia/Tokyo",
                        "Australia/Sydney",
                      ]),
                    ].map((zone) => (
                      <option key={zone} value={zone} />
                    ))}
                  </datalist>
                  <span className="field-hint">
                    An IANA name. The panel&rsquo;s zone is {PANEL_TIMEZONE};
                    an unknown name is refused.
                  </span>
                </>
              )}
              {key === "remindersList" && (
                <span className="field-hint">
                  Matched by exact name. Only the open count is read — their
                  words stay on the Mac.
                </span>
              )}
            </label>
          );
        }

        return (
          <p className="field-hint" key={key}>
            {humanise(key)} cannot be edited here yet.
          </p>
        );
  };

  const variantPicker =
    variantEntry && sources && now
      ? renderVariantPicker({
          field: describe(variantEntry[1]) as { kind: "layout-variant"; values: string[] },
          module,
          definition,
          value: String(options[variantEntry[0]] ?? ""),
          theme,
          sources,
          now,
          onChange: (next) => set(variantEntry[0], next),
        })
      : variantEntry
        ? renderField(variantEntry as [string, z.ZodTypeAny])
        : null;

  /*
   * Two surfaces, one pass over the schema.
   *
   * The default surface is the words and the picture: the wording of each text
   * role, and content controls like a list's rows or a countdown's date. The
   * Fine-tune fold is how those words are set — every typographic control and
   * every colour — plus the state and fallback wording a panel only draws when
   * data is missing, and whatever a module tagged advanced about itself. It is
   * collapsed, so choosing a component, writing its words and picking its look
   * is the whole of the first surface.
   */
  const primary: ReactNode[] = [];
  const fine: ReactNode[] = [];

  for (const [key, fieldSchema] of bodyEntries) {
    const field = describe(fieldSchema);

    if (field.kind === "text-element") {
      const element = options[key] as TextElement | undefined;
      if (!element || typeof element !== "object") continue;
      const defaultElement = defaults[key] as TextElement | undefined;
      const ml = maxLengthOf(fieldSchema);
      const shared = {
        fieldKey: key,
        label: humanise(key),
        value: element,
        ...(ml !== undefined ? { maxLength: ml } : {}),
        placeholders: placeholdersFor(defaultElement?.text),
        theme,
        notes: notes.filter((entry) => entry.role === key),
        contrasts: contrasts.filter((entry) => entry.role === key),
        onChange: (next: TextElement) => set(key, next),
      } as const;
      const overflowed = overflows.filter((entry) => entry.role === key);
      const overflowNote =
        overflowed.length > 0 ? (
          <p className="overflow-note" data-testid={`opt-${key}-overflow`}>
            {overflowed[0]?.kind === "width"
              ? "Too wide for its tile — marked in red on the panel, not cut."
              : "Too tall for its tile — marked in red on the panel, not cut."}
          </p>
        ) : null;

      if (FALLBACK_ROLES.has(key)) {
        // State wording is real, editable copy, but it is not what a person
        // came to write — so it lives with the rest of the fine print.
        fine.push(
          <div key={`${key}-fallback`}>
            <TextElementField {...shared} part="words" />
            <TextElementField {...shared} part="type" />
          </div>,
        );
      } else {
        primary.push(
          <div key={key}>
            <TextElementField {...shared} part="words" />
            {overflowNote}
          </div>,
        );
        fine.push(<TextElementField key={`${key}-type`} {...shared} part="type" />);
      }
      continue;
    }

    const node = renderField([key, fieldSchema] as [string, z.ZodTypeAny]);
    if (node === null || node === undefined) continue;
    if (
      isAdvanced(fieldSchema) ||
      field.kind === "text-style" ||
      field.kind === "colour-token"
    ) {
      fine.push(node);
    } else {
      primary.push(node);
    }
  }

  return (
    <div data-testid="module-inspector">
      <p className="field-hint">{definition.description}</p>

      {variantPicker}
      {primary}

      {fine.length > 0 && (
        <Fold summary="Fine-tune" testId="inspector-finetune">
          <p className="field-hint">Type, colour and the fine print.</p>
          {fine}
        </Fold>
      )}

      {onFrameChange && (
        <Fold summary="Separators" testId="inspector-separators">
          <FrameControls frame={module.frame} onChange={onFrameChange} />
        </Fold>
      )}

      {problems.length > 0 && (
        <ul className="error-note" data-testid="inspector-problems">
          {problems.map((problem) => (
            <li key={problem}>{problem}</li>
          ))}
        </ul>
      )}
    </div>
  );
}

/** Plainer names for the dispositions than the schema's own tokens. */
const VARIANT_LABEL: Record<string, string> = {
  underline: "Underline",
  banner: "Banner",
  sidebar: "Sidebar",
  wash: "Wash",
  arc: "Arc",
  horizon: "Horizon",
  duo: "Sun & moon",
};

const VARIANT_HINT: Record<string, string> = {
  underline: "Headline over a swept colour bar. The editorial default.",
  banner: "A colour band up top, the headline large below.",
  sidebar: "A tall colour block beside the type.",
  wash: "A soft tint behind centred type.",
  arc: "The sun on its arc over a graded sky.",
  horizon: "A low sky band with the readings large below.",
  duo: "The sun on the left, the moon on the right.",
};

function renderVariantPicker({
  field,
  module,
  definition,
  value,
  theme,
  sources,
  now,
  onChange,
}: {
  field: { kind: "layout-variant"; values: string[] };
  module: ModuleInstance;
  definition: ReturnType<typeof moduleDefinition>;
  value: string;
  theme: DashboardTheme;
  sources: DashboardSources;
  now: Date;
  onChange: (next: string) => void;
}) {
  const span = definition.defaultSpan;
  // Lay the module across the panel so its disposition reads in the thumbnail.
  const w = Math.min(8, Math.max(span.w, 6));
  const h = Math.min(6, Math.max(span.h, 3));
  const options: PickerOption<string>[] = field.values.map((variantValue) => ({
    value: variantValue,
    label: VARIANT_LABEL[variantValue] ?? humanise(variantValue),
    ...(VARIANT_HINT[variantValue] ? { hint: VARIANT_HINT[variantValue] } : {}),
    doc: thumbnailDoc(
      {
        ...module,
        options: { ...(module.options as Record<string, unknown>), variant: variantValue },
      },
      { x: 0, y: 0, w, h },
      theme,
    ),
  }));
  return (
    <VisualPicker
      legend="Disposition"
      value={value}
      options={options}
      sources={sources}
      now={now}
      onChange={onChange}
      testId="variant-picker"
    />
  );
}
