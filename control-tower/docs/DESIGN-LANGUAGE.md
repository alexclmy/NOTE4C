# The visual language

An editorial / risograph aesthetic for the NOTE4C panel: bold type, warm
textured colour fields, a printed-poster feel — made honest by the fact that
every surface is drawn by the same renderer that packs the device bytes, in the
panel's own four inks and no others.

This document is the reference for iteration 1: the aesthetic, the dither
engine and its 2 px rule, the two hero modules, the dashboard-wide Expression
controls, and the designer refit that made all of it choosable by eye.

## The panel is physical

Four inks — black `0`, white `1`, yellow `2`, red `3` — and nothing between
them. `pack()` (`src/core/render/frame.ts`) turns a frame into exactly 30000
bytes and throws on any other value; that throw is the acceptance gate for
everything here. There is no fifth colour and no grey. What gives a four-colour
panel the warmth of a printed poster is *tone*: an ink laid down through an
ordered pattern so a field reads as 40 % or 70 % coverage rather than as a flat
solid. That is what the dither engine is for.

## The dither engine

`src/core/render/dither.ts`. Pure TypeScript, no Node APIs, no `Math.random`,
no clock: every mark is a function of its position through an ordered matrix, so
the browser preview and the packed bytes are identical to the pixel, and two
renders of the same input are byte-identical. Matrices are anchored to global
frame coordinates, so two fields that meet share one continuous texture instead
of showing a seam.

### Primitives

```ts
// A flat toned rectangle.
fillRectDither(fb, rect, pigment, coverage, style, background?)

// A tonal band whose coverage ramps from one edge to the other.
fillBandDither(fb, rect, pigment, { from, to, axis: "x"|"y", style, background? })

// A filled disc (sun / moon), optionally shaded lighter at the rim.
fillDiscDither(fb, cx, cy, radius, pigment, coverage, style, { background?, edgeSoftness? })

// The general fill everything above is built on: a rect, optionally masked to a
// shape by an `inside(x,y)` predicate, with `tone` a number or a function of
// position for gradients and shaded shapes.
ditherRect(fb, rect, { pigment, tone, brush, texture, background?, inside? })
```

`style` is `{ brush, texture }`. `coverage`/`tone` is `0..1`.

### Brushes

Three, each a different way of turning coverage into marks:

- **Grain** — a Bayer 8×8 dispersed matrix. A fine, even risograph speckle.
- **Halftone** — a clustered-dot 8×8 matrix. Dots grow from their centres as a
  tone deepens, the look of a halftoned newspaper photograph.
- **Grid** — an ordered crosshatch. Two families of diagonal lines (~0.44
  coverage on their own) that a deepening tone thickens; a letterpress hatch.

### Pixel texture

The cell size, exposed as **Fine / Medium / Large**. `cellFor(pigment, texture)`
returns the side of one atomic cell — every mark is a solid block of that size.

### The 2 px rule, enforced two ways

RED and YELLOW never render finer than a 2 px cell, because a single isolated
warm pixel does not develop on this panel — a 1 px red speck is a dropout that
reads as damage. Black and white may be single pixels; the panel prints them.
This is not a guideline, it is enforced structurally:

1. **Up front.** `cellFor()` clamps the cell of an accent pigment to at least
   `MIN_ACCENT_CELL` (2). Change the base cell sizes however you like; an accent
   can never come out below 2×2. Black/white go to 1 px at the finest texture.
2. **After the fact.** Black ink drawn *over* a warm dithered shape — a sun's
   ring, a rule, type over a wash — can clip an otherwise-legal 2×2 accent block
   down to a lone corner pixel. `scrubIsolatedAccents()` removes any accent
   pixel with no same-pigment neighbour (8-connected). Both hero modules run it
   over their whole tile as the last drawing step. It never touches black or
   white.

`countIsolatedAccents(fb, rect?)` returns how many accent pixels break the rule;
`tests/unit/dither.test.ts` asserts it is **0** for every brush × texture ×
tone, for flat fields, for a curved disc mask, and for a gradient band, and the
hero-module tests assert it for every variant × colour stance.

### Colour budget

`accentBudget(colourUse)` says how much warm ink a stance will spend (a coverage
multiplier and a ceiling), and whether it spends any at all. "Black & white"
returns zero; "Expressive" gives the pigment room but keeps the ceiling below a
flat solid, so even the boldest field keeps its printed tooth.

## The astronomy

`src/core/render/astronomy.ts`. Pure closed-form maths, computed on the Mac from
a latitude, a longitude and the clock — no network source, and there could not
be one, because this is geometry:

- `sunDay(date, lat, lon)` — sunrise, sunset, solar noon, day length; names the
  polar-day and polar-night cases rather than returning a broken time. Standard
  NOAA sunrise equation, good to about a minute.
- `dayLengthDeltaMinutes(...)` — today's daylight minus yesterday's, the number
  that makes a sky panel feel like it is about the season.
- `sunPosition(now, day)` — where the sun is on today's arc, `0` at sunrise, `1`
  at sunset.
- `moonPhase(date)` — illuminated fraction, waxing/waning, and a phase name.

Tested against a published almanac (London) in `tests/unit/astronomy.test.ts`.

## The hero modules

### Headline (`src/core/render/modules/headline.ts`)

Big editorial type — a manual headline, an optional kicker and subline — set
over a dithered colour field. Nothing is data-bound; the words are the owner's,
and overflow is marked in red on the panel and reported to the designer the
standard way, never silently cut. The field's brush, texture and colour follow
the dashboard Expression.

- **Dispositions** (`variant`): `underline` (headline over a swept colour bar —
  the default), `banner` (a colour band up top with the kicker knocked out in
  paper), `sidebar` (a tall colour block beside the type), `wash` (a soft tint
  behind centred type).
- **`palette`**: `warm` (red), `sun` (yellow), `ink` (black) — before the
  colour-use stance is applied.

### Sky (`src/core/render/modules/sky.ts`)

Sunrise, sunset, day length and its day-over-day delta, the sun on its arc at
today's position, and the moon's phase — over a graded, dithered sky.

- **Dispositions** (`variant`): `arc` (the sun on its arc over a full graded
  sky, times at the horizon ends, day length large — the default), `horizon` (a
  shallow sky band with the readings large below), `duo` (sun on the left, moon
  on the right).
- **Advanced options** (folded by default): `latitude`, `longitude`, `timeZone`,
  `showMoon`. Latitude and longitude are seeded from `NOTE4C_WEATHER_LATITUDE` /
  `NOTE4C_WEATHER_LONGITUDE` (falling back to Paris), the same pair the weather
  source reads.

**A note on time.** The day's numbers are read from the real clock, because a
new day is a new panel and is worth a refresh. The sun's live position on the
arc is read through `chromeNow`, so it freezes when a frame is being *hashed*
rather than shown — the sun creeping a few pixels an hour is not, on its own,
worth spending a panel refresh, the same judgement the timestamp chrome makes.

## Expression — the dashboard-wide look

`src/core/theme.ts`. Four controls that re-skin the whole panel at once, stored
on the theme (schema version bumped 4 → 5, migration `migrateDashboardDocV4toV5`
in `src/core/migrate.ts`). **Every default is inert:** only modules that opt into
the dither engine read Expression, and `largerText` is off, so a dashboard saved
before Expression existed renders the identical frame.

- **Colour use** — `blackwhite` / `balanced` / `expressive`. How much red and
  yellow the renderer spends. Expressive is never garish; the ceiling stays
  below a flat solid.
- **Brush** — `grain` / `halftone` / `grid`.
- **Pixel texture** — `fine` / `medium` / `large`.
- **Larger text** — bumps every text role one step up the size ladder, applied
  at the render boundary (`applyLargerText`), never stored.

## The designer, refit around picking by eye

The editing surface was rebuilt so a disposition or a colour treatment is chosen
by **thumbnail**, not by reading a label:

- `src/ui/VisualPicker.tsx` — a radio group whose options are real renders of the
  panel, made by the renderer, so a thumbnail cannot promise a look the panel
  will not print. Arrow-key navigable, 44 px-plus targets, wraps rather than
  scrolling sideways at 375 px.
- **Disposition picker** in `ModuleInspector`, driven off the `LAYOUT_VARIANT_TAG`
  a module's `variant` enum carries — schema-driven, so a new module that offers
  dispositions is picked up with no change to the inspector.
- **Appearance** in `ThemePanel` — Colour / Brush / Texture, each a strip of
  thumbnails of the current dashboard re-skinned by that one choice, plus a
  Larger text toggle.
- **Advanced fold** — options a module tags with `ADVANCED_OPTION_TAG` (the Sky
  coordinates and timezone) collapse behind one disclosure, so the default
  surface stays short: pick a disposition, pick a look, edit the words.

Everything keeps the paper/ink identity from `tokens.css`: 2 px ink borders,
hard shadows with no blur, zero radius; the selected swatch presses into its own
shadow, the paper convention for "this is the chosen object".

## Iteration 2

- A **Weather-hero** module: the current conditions as a dithered sky + a big
  temperature, the weather equivalent of the Headline.
- **Air quality** as a tonal band (green→red is not available, so a coverage
  ramp in a single accent).
- Seed the Sky location from the server so a freshly added tile picks up the
  configured coordinates without the browser/​server env caveat.
- Fold the legacy Theme prose (padding, dashboard font, palette) behind a
  "Layout & type" disclosure once the e2e assertions that drive those controls
  are updated to open it.
