import { z } from "zod";
import {
  BLACK,
  RED,
  WHITE,
  YELLOW,
  type PaletteIndex,
  type PaletteName,
} from "./palette";
import { FONT_FAMILY_IDS, FONT_WEIGHTS, isFontSize, stepFontSize } from "./font";

/**
 * Dashboard-wide theme: padding, typography and palette policy.
 *
 * Three rules hold everything here together.
 *
 *   * THE PALETTE IS PHYSICAL. Black, white, yellow and red are pigments in
 *     the panel, not a colour scheme. Nothing in this file can invent a fifth
 *     colour, and the only values an element may carry are the four pigments
 *     under a semantic name, or the word "inherit".
 *   * A DEFAULT MUST CHANGE NOTHING. Every field here defaults to the value
 *     that reproduces the rendering this build already produced, so a
 *     dashboard saved before the theme existed migrates forward and paints the
 *     same frame. The owner changes their panel when they ask to, not when
 *     they upgrade.
 *   * INHERITANCE IS EXPLICIT. A text role either names its own family or
 *     names "inherit". There is no third state and no silent rewrite: making
 *     every role follow the dashboard is a deliberate action with its own
 *     confirmation, not a side effect of picking a font.
 */

/** Marker so the inspector and the theme panel can find this schema. */
export const DASHBOARD_THEME_TAG = "dashboard-theme";

/**
 * Where a colour can come from.
 *
 * Four semantic tokens, each one of the panel's own pigments, plus
 * "inherit", which means "whatever the module decided this element is for".
 * Deliberately NOT hex, RGB, or a name the panel cannot print: a colour
 * picker on this product would offer 16 million lies.
 */
export const COLOUR_TOKENS = [
  "inherit",
  "ink",
  "paper",
  "accent",
  "highlight",
] as const;
export type ColourToken = (typeof COLOUR_TOKENS)[number];

/** Which pigment each semantic token is. This mapping is the contract. */
export const TOKEN_PIGMENT: Record<Exclude<ColourToken, "inherit">, PaletteIndex> =
  {
    ink: BLACK,
    paper: WHITE,
    accent: RED,
    highlight: YELLOW,
  };

export const COLOUR_TOKEN_LABEL: Record<ColourToken, string> = {
  inherit: "Inherit (what this element is for)",
  ink: "Ink (black)",
  paper: "Paper (white)",
  accent: "Accent (red)",
  highlight: "Highlight (yellow)",
};

export const ColourTokenSchema = z.enum(COLOUR_TOKENS);

/**
 * Resolve a token to a pigment.
 *
 * `fallback` is what the module itself chose for this element, which is why
 * "inherit" is the default everywhere: a migrated dashboard keeps exactly the
 * colours its modules were already drawing.
 */
export function colourFor(
  token: ColourToken,
  fallback: PaletteIndex,
): PaletteIndex {
  return token === "inherit" ? fallback : TOKEN_PIGMENT[token];
}

/** "inherit" is not a value the panel can print, so name the pigment. */
export function tokenPigmentName(
  token: ColourToken,
  fallback: PaletteName,
): PaletteName {
  const names: Record<Exclude<ColourToken, "inherit">, PaletteName> = {
    ink: "black",
    paper: "white",
    accent: "red",
    highlight: "yellow",
  };
  return token === "inherit" ? fallback : names[token];
}

/**
 * The palette policy.
 *
 * Black and white are STRUCTURAL. They are typed as `true` rather than as
 * booleans so that turning one off is a schema error with a message, not a
 * setting that produces a blank or unreadable panel. The two accents are
 * The owner's to switch off, and switching one off does not hide the pigment: it
 * tells the renderer to stop asking for it, and every request is mapped to an
 * enabled colour instead.
 */
export const PalettePolicySchema = z.object({
  black: z
    .literal(true, {
      errorMap: () => ({
        message:
          "Black is structural and cannot be switched off: a panel with no ink has nothing on it",
      }),
    })
    .default(true),
  white: z
    .literal(true, {
      errorMap: () => ({
        message:
          "White is structural and cannot be switched off: it is the paper every module clears to",
      }),
    })
    .default(true),
  red: z.boolean().default(true),
  yellow: z.boolean().default(true),
});
export type PalettePolicy = z.infer<typeof PalettePolicySchema>;

/** The accents a policy can actually turn off. */
export const OPTIONAL_PIGMENTS = ["yellow", "red"] as const;
export const STRUCTURAL_PIGMENTS = ["black", "white"] as const;

export function pigmentEnabled(
  policy: PalettePolicy,
  index: PaletteIndex,
): boolean {
  if (index === RED) return policy.red;
  if (index === YELLOW) return policy.yellow;
  return true;
}

/**
 * Where a disabled pigment's ink goes instead.
 *
 * Deterministic and total, so the same dashboard and the same policy always
 * produce the same bytes:
 *
 *   red off    -> black. Black is always enabled, so this never chains.
 *   yellow off -> red when red is on, black when it is not. Yellow is an
 *                 accent; the nearest enabled accent is the honest
 *                 substitution, and black is the floor.
 *
 * Black and white are never remapped because they cannot be disabled.
 */
export function pigmentFallback(
  policy: PalettePolicy,
  index: PaletteIndex,
): PaletteIndex {
  if (index === RED && !policy.red) return BLACK;
  if (index === YELLOW && !policy.yellow) return policy.red ? RED : BLACK;
  return index;
}

/** Human wording for the fallback, for the UI. Never "the colour is gone". */
export function fallbackExplanation(policy: PalettePolicy): string[] {
  const lines: string[] = [];
  if (!policy.red) {
    lines.push(
      "Red is off, so every request for red is drawn in black. The panel's red pigment is still physically there; the renderer simply stops asking for it.",
    );
  }
  if (!policy.yellow) {
    lines.push(
      policy.red
        ? "Yellow is off, so every request for yellow is drawn in red, the nearest enabled accent."
        : "Yellow is off and red is off too, so every request for either is drawn in black.",
    );
  }
  return lines;
}

/**
 * Apply the policy to a finished frame, in place, and report how many pixels
 * moved.
 *
 * This runs as a final pass over the whole buffer rather than inside each
 * module, which is what makes the guarantee total: a module, a sprite, a
 * future module nobody has written yet, cannot put a disabled pigment on the
 * panel by forgetting to ask.
 */
export function applyPalettePolicy(
  pixels: Uint8Array,
  policy: PalettePolicy,
): number {
  if (policy.red && policy.yellow) return 0;
  const map: PaletteIndex[] = [
    pigmentFallback(policy, BLACK),
    pigmentFallback(policy, WHITE),
    pigmentFallback(policy, YELLOW),
    pigmentFallback(policy, RED),
  ];
  let moved = 0;
  for (let i = 0; i < pixels.length; i += 1) {
    const from = pixels[i] as PaletteIndex;
    const to = map[from] as PaletteIndex;
    if (to !== from) {
      pixels[i] = to;
      moved += 1;
    }
  }
  return moved;
}

/**
 * Content padding.
 *
 * Padding shrinks the usable area inside the fixed 400x300 frame and the 8x6
 * grid is redistributed across what is left, so the grid contract survives:
 * still eight columns, still six rows, still no module anywhere but on a cell
 * boundary. Nothing is cropped, because nothing is laid out past the content
 * rectangle in the first place.
 *
 * The ceiling is 24 px a side. At 24 the content area is 352x252 and a cell is
 * 44x42, which is still taller than the 34 px face's line box plus its own
 * padding. Past that the grid stops being usable rather than merely tight, so
 * the schema refuses it instead of rendering something that warns on every
 * tile.
 */
export const MIN_CONTENT_PADDING = 0;
export const MAX_CONTENT_PADDING = 24;

export const ContentPaddingSchema = z
  .number()
  .int({ message: "Content padding is a whole number of pixels" })
  .min(MIN_CONTENT_PADDING, {
    message: "Content padding cannot be negative",
  })
  .max(MAX_CONTENT_PADDING, {
    message: `Content padding stops at ${MAX_CONTENT_PADDING} px a side; past that an 8x6 cell is too small to set type in`,
  })
  .default(0);

/**
 * Dashboard typography.
 *
 * Family and weight only. Size is deliberately NOT global: every tile's
 * geometry was chosen against a size, and a dashboard-wide size would move
 * every module's layout at once, which is the opposite of the control this is
 * supposed to give. Size stays on the role, where the overflow warning that
 * goes with it also lives.
 */
export const DashboardTypographySchema = z.object({
  family: z.enum(FONT_FAMILY_IDS).default("inter"),
  weight: z.enum(FONT_WEIGHTS).default("regular"),
});
export type DashboardTypography = z.infer<typeof DashboardTypographySchema>;

/**
 * Expression: the dashboard-wide creative controls.
 *
 * These re-skin the whole panel at once. Unlike a module option, they are a
 * property of the composition, so a single move changes how every editorial
 * surface on the dashboard is set: how much of the panel's red and yellow the
 * renderer is willing to spend, which BRUSH the dithered colour fields are laid
 * down with, how coarse that texture is, and whether type is bumped a size up
 * for a reading-across-the-room panel.
 *
 * THE DEFAULTS CHANGE NOTHING THAT ALREADY EXISTS. Only the modules that opt
 * into the dither engine (Headline, Sky) read expression; every module written
 * before it renders the identical frame it did. `largerText` defaults off, so
 * even the one control that reaches every module is inert until asked for.
 *
 * The values are deliberately words, not numbers. "Expressive" is a stance the
 * renderer interprets against the physical panel — it spends accent ink with
 * intent, it does not crank a slider to garish — and a 0..100 knob would invite
 * exactly the over-saturated panel the words are chosen to prevent.
 */
export const COLOUR_USES = ["blackwhite", "balanced", "expressive"] as const;
export type ColourUse = (typeof COLOUR_USES)[number];

export const COLOUR_USE_LABEL: Record<ColourUse, string> = {
  blackwhite: "Black & white",
  balanced: "Balanced",
  expressive: "Expressive",
};

export const COLOUR_USE_NOTE: Record<ColourUse, string> = {
  blackwhite:
    "The editorial surfaces are set in ink on paper only. Red and yellow are held back entirely, for a stark newspaper look.",
  balanced:
    "Red and yellow are spent with restraint: an accent band, a warm sun, a single line that carries. The default.",
  expressive:
    "The warm pigments are given room — deeper colour fields, a fuller sky. Still intentional, never a rainbow: the panel has four inks and no fifth.",
};

export const BRUSHES = ["grain", "halftone", "grid"] as const;
export type Brush = (typeof BRUSHES)[number];

export const BRUSH_LABEL: Record<Brush, string> = {
  grain: "Grain",
  halftone: "Halftone",
  grid: "Grid",
};

export const BRUSH_NOTE: Record<Brush, string> = {
  grain: "A fine, even risograph speckle. The quietest of the three.",
  halftone: "Newsprint dots that grow from their centres as a tone deepens.",
  grid: "An ordered letterpress crosshatch. The most graphic, and the boldest.",
};

export const PIXEL_TEXTURES = ["fine", "medium", "large"] as const;
export type PixelTexture = (typeof PIXEL_TEXTURES)[number];

export const PIXEL_TEXTURE_LABEL: Record<PixelTexture, string> = {
  fine: "Fine",
  medium: "Medium",
  large: "Large",
};

export const PIXEL_TEXTURE_NOTE: Record<PixelTexture, string> = {
  fine: "The smallest cell the panel prints cleanly. Reads almost as a flat wash from across the room.",
  medium: "A visible tooth to every colour field. The default.",
  large: "Chunky, deliberate blocks. The most printed-poster of the three.",
};

/**
 * The one physical law encoded here: RED and YELLOW never render finer than a
 * 2 px cell, because a single isolated warm pixel does not develop on this
 * panel. The dither engine enforces it structurally; this constant is the
 * floor it clamps to, kept beside the words so the rule is where the theme is.
 */
export const MIN_ACCENT_CELL = 2;

export const ExpressionSchema = z
  .object({
    colourUse: z.enum(COLOUR_USES).default("balanced"),
    brush: z.enum(BRUSHES).default("grain"),
    pixelTexture: z.enum(PIXEL_TEXTURES).default("medium"),
    /**
     * Bump every text role one step up the size ladder. The one expression
     * control that reaches modules written before the dither engine, and the
     * reason it defaults off: a default that resized existing panels would be
     * the theme changing a frame nobody asked it to.
     */
    largerText: z.boolean().default(false),
  })
  .default({});
export type Expression = z.infer<typeof ExpressionSchema>;

export const DEFAULT_EXPRESSION: Expression = ExpressionSchema.parse({});

export const DashboardThemeSchema = z
  .object({
    contentPadding: ContentPaddingSchema,
    typography: DashboardTypographySchema.default({}),
    palette: PalettePolicySchema.default({}),
    expression: ExpressionSchema,
  })
  .describe(DASHBOARD_THEME_TAG);
export type DashboardTheme = z.infer<typeof DashboardThemeSchema>;

/** The theme a dashboard that has never had one gets. Visually inert. */
export const DEFAULT_THEME: DashboardTheme = DashboardThemeSchema.parse({});

/**
 * What a text style may say about its family and weight.
 *
 * "inherit" is a real, stored value rather than an absent field, so the
 * difference between "follows the dashboard" and "was set to Inter, which
 * happens to be the dashboard font today" is recorded and survives a change
 * to the dashboard font.
 */
export const INHERIT = "inherit" as const;
export const STYLE_FAMILIES = [...FONT_FAMILY_IDS, INHERIT] as const;
export const STYLE_WEIGHTS = [...FONT_WEIGHTS, INHERIT] as const;
export type StyleFamily = (typeof STYLE_FAMILIES)[number];
export type StyleWeight = (typeof STYLE_WEIGHTS)[number];

export function resolveFamily(
  family: StyleFamily,
  theme: DashboardTheme = DEFAULT_THEME,
): (typeof FONT_FAMILY_IDS)[number] {
  return family === INHERIT ? theme.typography.family : family;
}

export function resolveWeight(
  weight: StyleWeight,
  theme: DashboardTheme = DEFAULT_THEME,
): (typeof FONT_WEIGHTS)[number] {
  return weight === INHERIT ? theme.typography.weight : weight;
}

/**
 * Resolve every "inherit" in a module's parsed options against the theme.
 *
 * Done once at the render boundary rather than at every measurement, so the
 * text layer only ever sees a concrete family and weight and the two cannot
 * disagree about what a line is going to be set in.
 *
 * The walk looks for the one shape in this codebase that carries a `family`
 * key, which is a text style. Anything else is copied through untouched.
 */
export function resolveInheritedStyles<T>(value: T, theme: DashboardTheme): T {
  if (Array.isArray(value)) {
    return value.map((entry) => resolveInheritedStyles(entry, theme)) as unknown as T;
  }
  if (value === null || typeof value !== "object") return value;

  const source = value as Record<string, unknown>;
  const out: Record<string, unknown> = {};
  for (const [key, entry] of Object.entries(source)) {
    out[key] = resolveInheritedStyles(entry, theme);
  }
  if (typeof source.family === "string" && typeof source.size === "number") {
    out.family = resolveFamily(source.family as StyleFamily, theme);
    if (typeof source.weight === "string") {
      out.weight = resolveWeight(source.weight as StyleWeight, theme);
    }
  }
  return out as T;
}

/**
 * Bump every text role in an options object one step up the size ladder.
 *
 * This is the whole of the "Larger text" expression control. It runs at the
 * render boundary, after inheritance is resolved, on a dashboard that asked for
 * it — never on a stored document, so the choice is a way of looking at the
 * panel and not an edit to the words. A size already at the top of the ladder
 * stays there; `stepFontSize` clamps. Called only when the flag is on, so a
 * dashboard that did not ask keeps its exact sizes and its exact frame.
 */
export function applyLargerText<T>(value: T, enabled: boolean): T {
  if (!enabled) return value;
  if (Array.isArray(value)) {
    return value.map((entry) => applyLargerText(entry, enabled)) as unknown as T;
  }
  if (value === null || typeof value !== "object") return value;
  const source = value as Record<string, unknown>;
  const out: Record<string, unknown> = {};
  for (const [key, entry] of Object.entries(source)) {
    out[key] = applyLargerText(entry, enabled);
  }
  if (typeof source.family === "string" && isFontSize(source.size)) {
    out.size = stepFontSize(source.size, 1);
  }
  return out as T;
}

/** Count the text roles in a parsed options object, and how they inherit. */
export function countStyleInheritance(value: unknown): {
  total: number;
  inheriting: number;
  overridden: number;
} {
  let total = 0;
  let inheriting = 0;
  const walk = (node: unknown): void => {
    if (Array.isArray(node)) {
      node.forEach(walk);
      return;
    }
    if (node === null || typeof node !== "object") return;
    const record = node as Record<string, unknown>;
    if (typeof record.family === "string" && typeof record.size === "number") {
      total += 1;
      if (record.family === INHERIT) inheriting += 1;
    }
    Object.values(record).forEach(walk);
  };
  walk(value);
  return { total, inheriting, overridden: total - inheriting };
}

/**
 * Rewrite every text style in an options object to follow the dashboard.
 *
 * This is the "apply to all text roles" action, and it exists as a separate
 * function precisely because it is destructive of local choices: the UI asks
 * first, shows what would change, and only then calls this.
 */
export function applyFamilyToAllStyles<T>(value: T): T {
  if (Array.isArray(value)) {
    return value.map((entry) => applyFamilyToAllStyles(entry)) as unknown as T;
  }
  if (value === null || typeof value !== "object") return value;
  const source = value as Record<string, unknown>;
  const out: Record<string, unknown> = {};
  for (const [key, entry] of Object.entries(source)) {
    out[key] = applyFamilyToAllStyles(entry);
  }
  if (typeof source.family === "string" && typeof source.size === "number") {
    out.family = INHERIT;
  }
  return out as T;
}

/**
 * Foreground and background combinations that do not work on this panel.
 *
 * "invisible" is the same pigment on both sides: ink that cannot be read at
 * all. "low" is yellow against white, which on the physical panel is a pale
 * wash the owner will not read from across the room. Both are reported rather than
 * silently corrected: overriding a colour the user chose would be the renderer
 * inventing, which is the one thing this product does not do.
 */
export type ContrastVerdict = "invisible" | "low" | null;

export function contrastVerdict(
  foreground: PaletteIndex,
  background: PaletteIndex,
): ContrastVerdict {
  if (foreground === background) return "invisible";
  if (
    (foreground === YELLOW && background === WHITE) ||
    (foreground === WHITE && background === YELLOW)
  ) {
    return "low";
  }
  return null;
}
