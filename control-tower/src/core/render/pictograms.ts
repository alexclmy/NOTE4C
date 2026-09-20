import { BLACK, RED, WHITE, YELLOW, type PaletteIndex } from "@/core/palette";

/**
 * Pictograms: the panel's own answer to emoji.
 *
 * WHY NOT A COLOUR EMOJI FONT
 * ---------------------------
 * Apple Color Emoji is licensed to Apple and cannot be redistributed, and its
 * glyphs are full-colour bitmaps. This panel has four pigments and no network,
 * and its atlases are committed to git. Nothing about a colour emoji font fits
 * any of that. Noto Emoji would be redistributable but is a large download
 * this machine does not have offline, and a greyscale emoji face still has to
 * be thresholded into four colours by somebody.
 *
 * So these are drawn here, by hand, on a 12x12 grid, as source code. They are
 * ours to ship, they are deterministic, they are identical in Node and in the
 * browser, they need no asset and no font, and every pixel is already one of
 * the four pigments the device can print.
 *
 * WHAT IS AND IS NOT SUPPORTED
 * ----------------------------
 * A deliberately bounded set, listed below and documented in the inspector:
 * the household and reminder symbols that actually turn up on a fridge note.
 * Everything else is drawn as the UNSUPPORTED box, which is a visible
 * statement that a symbol was there, rather than a dropped character or a tofu
 * rectangle from a font that has no glyph. The designer is told which symbols
 * those were.
 *
 * Variation selectors and skin tone modifiers are stripped before lookup, so
 * the text and presentation forms of the same symbol resolve to one pictogram.
 * ZWJ sequences are NOT composed: each component is resolved on its own, which
 * is honest about what this set can do.
 */

/** Every pictogram is drawn on a 12 by 12 grid. */
export const PICTOGRAM_GRID = 12;

/**
 * The drawing alphabet.
 *   .  transparent, the paper shows through
 *   #  black, the ink
 *   o  white, for the inside of an outlined shape
 *   r  red
 *   y  yellow
 */
const PIXELS: Record<string, PaletteIndex | null> = {
  ".": null,
  "#": BLACK,
  o: WHITE,
  r: RED,
  y: YELLOW,
};

export const PICTOGRAM_CATEGORIES = [
  "weather",
  "heart",
  "check",
  "warning",
  "celebration",
  "food",
  "animals",
  "home",
  "transport",
  "clock",
  "marks",
] as const;
export type PictogramCategory = (typeof PICTOGRAM_CATEGORIES)[number];

export interface Pictogram {
  /** The primary emoji this draws, for documentation and the inspector. */
  emoji: string;
  name: string;
  category: PictogramCategory;
  /** 12 rows of 12 characters from the alphabet above. */
  rows: readonly string[];
}

/** Swap one colour for another. Used where two emoji share a shape. */
function recolour(rows: readonly string[], from: string, to: string): string[] {
  return rows.map((row) => row.split(from).join(to));
}

const HEART_ROWS = [
  "............",
  "..rr...rr...",
  ".rrrr.rrrr..",
  "rrrrrrrrrrr.",
  "rrrrrrrrrrr.",
  "rrrrrrrrrrr.",
  ".rrrrrrrrr..",
  "..rrrrrrr...",
  "...rrrrr....",
  "....rrr.....",
  ".....r......",
  "............",
] as const;

const CLOCK_FACE_ROWS = [
  "............",
  "..########..",
  ".#oooooooo#.",
  "#oooo#ooooo#",
  "#oooo#ooooo#",
  "#oooo###ooo#",
  "#oooooooooo#",
  "#oooooooooo#",
  ".#oooooooo#.",
  "..########..",
  "............",
  "............",
] as const;

const CAT_FACE_ROWS = [
  "............",
  ".#........#.",
  ".##......##.",
  ".###....###.",
  ".##########.",
  ".#o#oooo#o#.",
  ".#oooooooo#.",
  ".#oo#oo#oo#.",
  ".#oooooooo#.",
  "..########..",
  "............",
  "............",
] as const;

/**
 * The set. Keyed by the emoji's codepoints with variation selectors already
 * removed, which is the same normalisation `pictogramKey` applies to input.
 */
const ENTRIES: Pictogram[] = [
  {
    emoji: "☀",
    name: "Sun",
    category: "weather",
    rows: [
      ".....##.....",
      "............",
      "....####....",
      "...#yyyy#...",
      "..#yyyyyy#..",
      "#.#yyyyyy#.#",
      "#.#yyyyyy#.#",
      "..#yyyyyy#..",
      "...#yyyy#...",
      "....####....",
      "............",
      ".....##.....",
    ],
  },
  {
    emoji: "☁",
    name: "Cloud",
    category: "weather",
    rows: [
      "............",
      "............",
      "....####....",
      "...#oooo#...",
      "..#oooooo##.",
      ".#oooooooo#.",
      "#oooooooooo#",
      "#oooooooooo#",
      ".##########.",
      "............",
      "............",
      "............",
    ],
  },
  {
    emoji: "🌧",
    name: "Rain",
    category: "weather",
    rows: [
      "...####.....",
      "..#oooo##...",
      ".#oooooo#...",
      "#oooooooo##.",
      "#oooooooooo#",
      ".##########.",
      "............",
      "..#...#...#.",
      "..#...#...#.",
      ".#...#...#..",
      ".#...#...#..",
      "............",
    ],
  },
  {
    emoji: "❄",
    name: "Snowflake",
    category: "weather",
    rows: [
      ".....##.....",
      "..#..##..#..",
      "...#.##.#...",
      "....####....",
      "#....##....#",
      ".##########.",
      ".##########.",
      "#....##....#",
      "....####....",
      "...#.##.#...",
      "..#..##..#..",
      ".....##.....",
    ],
  },
  {
    emoji: "⛈",
    name: "Storm",
    category: "weather",
    rows: [
      "...####.....",
      "..#oooo##...",
      ".#oooooo#...",
      "#oooooooo##.",
      "#oooooooooo#",
      ".##########.",
      ".....yyy....",
      "....yyy.....",
      "...yyyyy....",
      "....yyy.....",
      "...yy.......",
      "..yy........",
    ],
  },
  {
    emoji: "🌙",
    name: "Moon",
    category: "weather",
    rows: [
      "....###.....",
      "..###.......",
      ".###........",
      ".###........",
      "###.........",
      "###.........",
      "###.........",
      "###.........",
      ".###........",
      ".###........",
      "..###.......",
      "....###.....",
    ],
  },
  { emoji: "❤", name: "Red heart", category: "heart", rows: HEART_ROWS },
  {
    emoji: "💛",
    name: "Yellow heart",
    category: "heart",
    rows: recolour(HEART_ROWS, "r", "y"),
  },
  {
    emoji: "✅",
    name: "Check",
    category: "check",
    rows: [
      "............",
      "..........##",
      ".........##.",
      "........##..",
      ".#.....##...",
      ".##...##....",
      "..##.##.....",
      "...####.....",
      "....##......",
      "............",
      "............",
      "............",
    ],
  },
  {
    emoji: "❌",
    name: "Cross",
    category: "check",
    rows: [
      "............",
      ".rr......rr.",
      "..rr....rr..",
      "...rr..rr...",
      "....rrrr....",
      ".....rr.....",
      ".....rr.....",
      "....rrrr....",
      "...rr..rr...",
      "..rr....rr..",
      ".rr......rr.",
      "............",
    ],
  },
  {
    emoji: "⚠",
    name: "Warning",
    category: "warning",
    rows: [
      "............",
      ".....##.....",
      "....#yy#....",
      "....#yy#....",
      "...#y##y#...",
      "...#y##y#...",
      "..#yy##yy#..",
      "..#yyyyyy#..",
      ".#yyy##yyy#.",
      ".#yyyyyyyy#.",
      ".##########.",
      "............",
    ],
  },
  {
    emoji: "❗",
    name: "Exclamation",
    category: "warning",
    rows: [
      "............",
      "....rrrr....",
      "....rrrr....",
      "....rrrr....",
      "....rrrr....",
      "....rrrr....",
      "....rrrr....",
      "....rrrr....",
      "............",
      "....rrrr....",
      "....rrrr....",
      "............",
    ],
  },
  {
    emoji: "🎉",
    name: "Party popper",
    category: "celebration",
    rows: [
      ".........r..",
      "......y..y..",
      "........r...",
      "....r..y....",
      "......#.....",
      ".....##.....",
      "....###.....",
      "...####.....",
      "..#####.....",
      ".######.....",
      "#######.....",
      "............",
    ],
  },
  {
    emoji: "🎂",
    name: "Cake",
    category: "celebration",
    rows: [
      ".....r......",
      ".....#......",
      ".....#......",
      "..########..",
      "..#oooooo#..",
      "..########..",
      "..#oooooo#..",
      "..#oooooo#..",
      "..########..",
      "............",
      "............",
      "............",
    ],
  },
  {
    emoji: "🎈",
    name: "Balloon",
    category: "celebration",
    rows: [
      "...rrrr.....",
      "..rrrrrr....",
      ".rrrrrrrr...",
      ".rrrrrrrr...",
      "..rrrrrr....",
      "...rrrr.....",
      "....rr......",
      ".....#......",
      "....#.......",
      ".....#......",
      "....#.......",
      "............",
    ],
  },
  {
    emoji: "🥚",
    name: "Egg",
    category: "food",
    rows: [
      "............",
      "....###.....",
      "...#ooo#....",
      "..#ooooo#...",
      "..#ooooo#...",
      ".#ooooooo#..",
      ".#ooooooo#..",
      ".#ooooooo#..",
      "..#ooooo#...",
      "...#####....",
      "............",
      "............",
    ],
  },
  {
    emoji: "🍞",
    name: "Bread",
    category: "food",
    rows: [
      "............",
      "...######...",
      "..########..",
      ".#oo#oo#oo#.",
      "#oooooooooo#",
      "#oooooooooo#",
      "#oooooooooo#",
      "#oooooooooo#",
      ".##########.",
      "............",
      "............",
      "............",
    ],
  },
  {
    emoji: "☕",
    name: "Coffee",
    category: "food",
    rows: [
      "............",
      "............",
      "..y..y......",
      "...y..y.....",
      ".########...",
      ".#oooooo#.##",
      ".#oooooo#.#.",
      ".#oooooo###.",
      "..#oooo#....",
      "...####.....",
      "............",
      "............",
    ],
  },
  {
    emoji: "🍎",
    name: "Apple",
    category: "food",
    rows: [
      "......#.....",
      ".....#......",
      "..rrr.rrr...",
      ".rrrrrrrrr..",
      "rrrrrrrrrrr.",
      "rrrrrrrrrrr.",
      "rrrrrrrrrrr.",
      "rrrrrrrrrrr.",
      ".rrrrrrrrr..",
      "..rrrrrrr...",
      "...rr.rr....",
      "............",
    ],
  },
  {
    emoji: "🥛",
    name: "Milk",
    category: "food",
    rows: [
      "............",
      ".##########.",
      ".#oooooooo#.",
      ".#oooooooo#.",
      ".#oooooooo#.",
      "..#oooooo#..",
      "..#oooooo#..",
      "...#oooo#...",
      "...#oooo#...",
      "....####....",
      "............",
      "............",
    ],
  },
  {
    emoji: "🐔",
    name: "Hen",
    category: "animals",
    rows: [
      "...rr.......",
      "..rrrr......",
      ".######.yy..",
      ".#o####.yy..",
      ".#######y...",
      "..######....",
      ".########...",
      "##########..",
      "##########..",
      ".########...",
      "..#....#....",
      ".yy....yy...",
    ],
  },
  {
    emoji: "🐥",
    name: "Chick",
    category: "animals",
    rows: [
      "............",
      "...yyyy.....",
      "..yyyyyy....",
      "..yy#yyy.rr.",
      "..yyyyyyyrr.",
      "..yyyyyy....",
      ".yyyyyyyy...",
      ".yyyyyyyy...",
      ".yyyyyyyy...",
      "..yyyyyy....",
      "...y..y.....",
      "............",
    ],
  },
  {
    emoji: "🐶",
    name: "Dog",
    category: "animals",
    rows: [
      "............",
      ".##......##.",
      ".###....###.",
      ".##########.",
      ".#o#oooo#o#.",
      ".#oooooooo#.",
      ".#ooo##ooo#.",
      ".#oo####oo#.",
      ".#oooooooo#.",
      "..########..",
      "............",
      "............",
    ],
  },
  { emoji: "🐱", name: "Cat", category: "animals", rows: CAT_FACE_ROWS },
  {
    emoji: "🏠",
    name: "House",
    category: "home",
    rows: [
      "............",
      ".....##.....",
      "....####....",
      "...######...",
      "..########..",
      ".##########.",
      ".#oooooooo#.",
      ".#oo####oo#.",
      ".#oo#oo#oo#.",
      ".#oo#oo#oo#.",
      ".##########.",
      "............",
    ],
  },
  {
    emoji: "🧹",
    name: "Broom",
    category: "home",
    rows: [
      "..........#.",
      ".........#..",
      "........#...",
      ".......#....",
      "......#.....",
      ".....#......",
      "....#.......",
      "..yyyy......",
      ".yyyyyy.....",
      "yyyyyyyy....",
      "y.y.y.y.y...",
      "............",
    ],
  },
  {
    emoji: "🧺",
    name: "Basket",
    category: "home",
    rows: [
      "............",
      "...######...",
      "..#......#..",
      ".##########.",
      ".#oo#oo#oo#.",
      ".#o#oo#oo#o.",
      ".#oo#oo#oo#.",
      ".#o#oo#oo#o.",
      "..########..",
      "............",
      "............",
      "............",
    ],
  },
  {
    emoji: "🚗",
    name: "Car",
    category: "transport",
    rows: [
      "............",
      "............",
      "....#####...",
      "...#ooooo##.",
      "..#ooooooo#.",
      "############",
      "#oooooooooo#",
      "############",
      "..##....##..",
      "..##....##..",
      "............",
      "............",
    ],
  },
  {
    emoji: "🚲",
    name: "Bicycle",
    category: "transport",
    rows: [
      "............",
      "............",
      "............",
      "............",
      "........##..",
      "..#####.#...",
      ".###..#.###.",
      "#...#.##...#",
      "#...#..#...#",
      "#...#..#...#",
      ".###....###.",
      "............",
    ],
  },
  {
    emoji: "🚌",
    name: "Bus",
    category: "transport",
    rows: [
      "............",
      ".##########.",
      ".#oooooooo#.",
      ".#o##o##oo#.",
      ".#o##o##oo#.",
      ".#oooooooo#.",
      ".##########.",
      ".#oooooooo#.",
      ".##########.",
      "..##....##..",
      "............",
      "............",
    ],
  },
  {
    emoji: "⏰",
    name: "Alarm clock",
    category: "clock",
    rows: [
      ".#........#.",
      "..########..",
      ".#oooooooo#.",
      "#oooo#ooooo#",
      "#oooo#ooooo#",
      "#oooo###ooo#",
      "#oooooooooo#",
      "#oooooooooo#",
      ".#oooooooo#.",
      "..########..",
      "..#......#..",
      "............",
    ],
  },
  { emoji: "🕐", name: "Clock", category: "clock", rows: CLOCK_FACE_ROWS },
  {
    emoji: "⏳",
    name: "Hourglass",
    category: "clock",
    rows: [
      "............",
      ".##########.",
      ".#yyyyyyyy#.",
      "..#yyyyyy#..",
      "...#yyyy#...",
      "....#yy#....",
      "....#yy#....",
      "...#o..o#...",
      "..#oyyyyo#..",
      ".#yyyyyyyy#.",
      ".##########.",
      "............",
    ],
  },
  {
    emoji: "⭐",
    name: "Star",
    category: "marks",
    rows: [
      ".....yy.....",
      ".....yy.....",
      "....yyyy....",
      "yyyyyyyyyyyy",
      ".yyyyyyyyyy.",
      "..yyyyyyyy..",
      "...yyyyyy...",
      "..yyyyyyyy..",
      "..yyy..yyy..",
      ".yy......yy.",
      "............",
      "............",
    ],
  },
  {
    emoji: "📌",
    name: "Pin",
    category: "marks",
    rows: [
      "............",
      "....rrrr....",
      "...rrrrrr...",
      "...rrrrrr...",
      "...rrrrrr...",
      "....rrrr....",
      ".....##.....",
      ".....##.....",
      ".....##.....",
      ".....#......",
      "............",
      "............",
    ],
  },
  {
    emoji: "🍁",
    name: "Maple leaf",
    category: "marks",
    rows: [
      ".....r......",
      "..r..r..r...",
      "..r.rrr.r...",
      ".rr.rrr.rr..",
      "..rrrrrrr...",
      "rrrrrrrrrrr.",
      ".rrrrrrrrr..",
      "..rrrrrrr...",
      "...rrrrr....",
      "....r.r.....",
      ".....r......",
      ".....r......",
    ],
  },
  {
    emoji: "💧",
    name: "Droplet",
    category: "marks",
    rows: [
      "............",
      ".....#......",
      "....#.#.....",
      "...#...#....",
      "..#.....#...",
      "..#.....#...",
      ".#.......#..",
      ".#.......#..",
      "..#.....#...",
      "...#####....",
      "............",
      "............",
    ],
  },
  {
    emoji: "🔑",
    name: "Key",
    category: "marks",
    rows: [
      "............",
      "............",
      "............",
      "..####......",
      ".#oooo#.....",
      ".#o##o#####.",
      ".#oooo#.#.#.",
      "..####......",
      "............",
      "............",
      "............",
      "............",
    ],
  },
  {
    emoji: "📅",
    name: "Calendar",
    category: "marks",
    rows: [
      "............",
      "..#....#....",
      ".##########.",
      ".##########.",
      ".#oooooooo#.",
      ".#o#o#o#oo#.",
      ".#oooooooo#.",
      ".#o#o#o#oo#.",
      ".#oooooooo#.",
      ".##########.",
      "............",
      "............",
    ],
  },
];

/**
 * Drawn for a symbol this set does not cover.
 *
 * A question mark inside a box, in the attention colour. It is deliberately
 * unlike any supported pictogram and unlike ordinary text: it says "a symbol
 * was written here and this panel cannot draw it", which is the one thing a
 * dropped character and a tofu rectangle both fail to say.
 */
export const UNSUPPORTED_PICTOGRAM: Pictogram = {
  emoji: "",
  name: "Unsupported symbol",
  category: "marks",
  rows: [
    "............",
    "..########..",
    "..#......#..",
    "..#.rrrr.#..",
    "..#.r..r.#..",
    "..#....r.#..",
    "..#...r..#..",
    "..#...r..#..",
    "..#......#..",
    "..#...r..#..",
    "..########..",
    "............",
  ],
};

/**
 * Aliases, so the presentation variants and the near-synonyms of a symbol land
 * on the same drawing instead of on the unsupported box.
 */
const ALIASES: Record<string, string> = {
  "✔": "✅",
  "☑": "✅",
  "✓": "✅",
  "✖": "❌",
  "✗": "❌",
  "❎": "✅",
  "🌤": "☁",
  "⛅": "☁",
  "🌥": "☁",
  "☔": "🌧",
  "🌦": "🌧",
  "⛄": "❄",
  "🌟": "⭐",
  "✨": "⭐",
  "💖": "❤",
  "🧡": "❤",
  "💚": "💛",
  "💙": "❤",
  "🩷": "❤",
  "‼": "❗",
  "❕": "❗",
  "⁉": "❗",
  "🐓": "🐔",
  "🐤": "🐥",
  "🐣": "🐥",
  "🐕": "🐶",
  "🐈": "🐱",
  "🏡": "🏠",
  "🏘": "🏠",
  "🥯": "🍞",
  "🥖": "🍞",
  "🍏": "🍎",
  "🫖": "☕",
  "🍵": "☕",
  "🚙": "🚗",
  "🚕": "🚗",
  "🚐": "🚌",
  "🚎": "🚌",
  "⏱": "⏰",
  "⌚": "🕐",
  "🕑": "🕐",
  "🕒": "🕐",
  "🕓": "🕐",
  "🕔": "🕐",
  "🕕": "🕐",
  "🕖": "🕐",
  "🕗": "🕐",
  "🕘": "🕐",
  "🕙": "🕐",
  "🕚": "🕐",
  "🕛": "🕐",
  "⌛": "⏳",
  "🗓": "📅",
  "📆": "📅",
  "🎊": "🎉",
  "🥳": "🎉",
  "🧽": "🧹",
  "🧼": "🧹",
  "🗝": "🔑",
  "💦": "💧",
  "📍": "📌",
};

export const PICTOGRAMS: Record<string, Pictogram> = Object.fromEntries(
  ENTRIES.map((entry) => [entry.emoji, entry]),
);

/**
 * Codepoints dropped before lookup: the two variation selectors, the five skin
 * tone modifiers, and the zero width joiner. None of them is a symbol on its
 * own, and none of them can change what this panel is able to draw.
 */
const IGNORED_CODEPOINTS = new Set<number>([
  0xfe0e, 0xfe0f, 0x200d, 0x1f3fb, 0x1f3fc, 0x1f3fd, 0x1f3fe, 0x1f3ff,
]);

export function isIgnoredCodepoint(codepoint: number): boolean {
  return IGNORED_CODEPOINTS.has(codepoint);
}

/** Resolve a single character to a pictogram, following aliases. */
export function pictogramFor(char: string): Pictogram | undefined {
  const direct = PICTOGRAMS[char];
  if (direct) return direct;
  const alias = ALIASES[char];
  return alias ? PICTOGRAMS[alias] : undefined;
}

/**
 * The Unicode blocks that hold symbols and emoji rather than letters.
 *
 * Listed explicitly rather than approximated with "anything above U+2000",
 * which would call a Han character a symbol. The distinction matters on the
 * panel: an unsupported SYMBOL gets the box, because something pictorial was
 * meant and the reader should know it is missing, while an unsupported LETTER
 * gets the question mark this renderer has always drawn, because that is a
 * gap in the face rather than a picture the panel cannot make.
 */
const SYMBOL_RANGES: ReadonlyArray<readonly [number, number]> = [
  [0x203c, 0x203c], // double exclamation
  [0x2049, 0x2049], // exclamation question
  [0x2100, 0x27bf], // letterlike, arrows, maths, misc symbols, dingbats
  [0x2900, 0x297f], // supplemental arrows
  [0x2b00, 0x2bff], // misc symbols and arrows, including the star
  [0x3030, 0x303d], // wavy dash, part alternation mark
  [0x1f000, 0x1faff], // the emoji planes
];

/** Does this character look like a symbol somebody drew, rather than a letter? */
export function looksLikeSymbol(char: string): boolean {
  const codepoint = char.codePointAt(0) ?? 0;
  return SYMBOL_RANGES.some(([low, high]) => codepoint >= low && codepoint <= high);
}

/**
 * How many times to repeat each pictogram pixel.
 *
 * One step, at 22 px and up, because a 12 px drawing beside a 27 px capital
 * reads as a typo. Integer scaling only: a resampled pictogram would be a
 * different picture at every size and would not survive the parity check.
 */
export function pictogramScale(fontSize: number): number {
  return fontSize >= 22 ? 2 : 1;
}

export interface PictogramMask {
  w: number;
  h: number;
  /** One palette index per pixel, or 255 for transparent. */
  data: Uint8Array;
}

export const TRANSPARENT = 255;

const maskCache = new Map<string, PictogramMask>();

/** Decode a pictogram to palette indices at a scale. Cached per shape. */
export function pictogramMask(picto: Pictogram, scale: number): PictogramMask {
  const step = Math.max(1, Math.trunc(scale));
  const key = `${picto.emoji || picto.name}/${step}`;
  const cached = maskCache.get(key);
  if (cached) return cached;

  const w = PICTOGRAM_GRID * step;
  const h = PICTOGRAM_GRID * step;
  const data = new Uint8Array(w * h).fill(TRANSPARENT);
  for (let row = 0; row < PICTOGRAM_GRID; row += 1) {
    const line = picto.rows[row] ?? "";
    for (let col = 0; col < PICTOGRAM_GRID; col += 1) {
      const value = PIXELS[line[col] ?? "."];
      if (value === null || value === undefined) continue;
      for (let dy = 0; dy < step; dy += 1) {
        for (let dx = 0; dx < step; dx += 1) {
          data[(row * step + dy) * w + (col * step + dx)] = value;
        }
      }
    }
  }
  const mask: PictogramMask = { w, h, data };
  maskCache.set(key, mask);
  return mask;
}

/** Every supported symbol, grouped, for the inspector and the report. */
export function supportedPictograms(): Array<{
  category: PictogramCategory;
  entries: Array<{ emoji: string; name: string }>;
}> {
  return PICTOGRAM_CATEGORIES.map((category) => ({
    category,
    entries: ENTRIES.filter((entry) => entry.category === category).map(
      (entry) => ({ emoji: entry.emoji, name: entry.name }),
    ),
  })).filter((group) => group.entries.length > 0);
}

/** Aliases, so the documentation can say which forms land where. */
export function pictogramAliases(): Array<{ from: string; to: string }> {
  return Object.entries(ALIASES).map(([from, to]) => ({ from, to }));
}

/** A sanity check the test suite runs over every shape. */
export function pictogramProblems(): string[] {
  const problems: string[] = [];
  const check = (picto: Pictogram): void => {
    const label = picto.emoji || picto.name;
    if (picto.rows.length !== PICTOGRAM_GRID) {
      problems.push(`${label} has ${picto.rows.length} rows, expected ${PICTOGRAM_GRID}`);
    }
    picto.rows.forEach((row, index) => {
      if (row.length !== PICTOGRAM_GRID) {
        problems.push(
          `${label} row ${index} is ${row.length} wide, expected ${PICTOGRAM_GRID}`,
        );
      }
      for (const char of row) {
        if (!(char in PIXELS)) {
          problems.push(`${label} row ${index} uses "${char}", which is not a pigment`);
        }
      }
    });
    if (picto.rows.every((row) => /^\.*$/.test(row))) {
      problems.push(`${label} is blank`);
    }
  };
  ENTRIES.forEach(check);
  check(UNSUPPORTED_PICTOGRAM);

  for (const [from, to] of Object.entries(ALIASES)) {
    if (!PICTOGRAMS[to]) problems.push(`Alias ${from} points at unknown ${to}`);
    if (PICTOGRAMS[from]) problems.push(`Alias ${from} shadows a real pictogram`);
  }
  return problems;
}
