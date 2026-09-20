import atkinsonFile from "./atkinson.json";
import interFile from "./inter.json";
import plexmonoFile from "./plexmono.json";
import poppinsFile from "./poppins.json";
import manifestFile from "./manifest.json";
import {
  FontAtlasSchema,
  atlasKey,
  type FontAtlas,
  type FontFamilyId,
  type FontWeight,
} from "@/core/font";

/**
 * Atlas loading.
 *
 * The committed atlas files are large and fully machine generated. Validating
 * all 56 atlases eagerly would cost real time on every cold start for no
 * benefit, so each atlas is validated once, the first time it is asked for.
 *
 * The imports are static rather than dynamic on purpose: the designer preview
 * renders synchronously with the same code path the server packs bytes with,
 * and an awaited font load would fork those two paths.
 */

interface RawFamilyFile {
  schema_version: number;
  family: string;
  label: string;
  raster: string;
  licence: string;
  atlases: Record<string, unknown>;
}

const FILES: Record<FontFamilyId, RawFamilyFile> = {
  inter: interFile as unknown as RawFamilyFile,
  atkinson: atkinsonFile as unknown as RawFamilyFile,
  plexmono: plexmonoFile as unknown as RawFamilyFile,
  poppins: poppinsFile as unknown as RawFamilyFile,
};

export interface FontFamilyMeta {
  label: string;
  note: string;
  licence: string;
  copyright: string;
  upstream: string;
  vendored_from: string;
  licence_file: string;
  faces: Record<string, { path: string; sha256: string }>;
}

interface RawManifest {
  schema_version: number;
  raster: string;
  sizes: number[];
  weights: string[];
  families: Record<string, FontFamilyMeta>;
}

const manifest = manifestFile as unknown as RawManifest;

/** How the glyphs were produced, for Diagnostics and the report. */
export const ATLAS_RASTER = manifest.raster;

export const FONT_FAMILY_META: Record<FontFamilyId, FontFamilyMeta> =
  manifest.families as Record<FontFamilyId, FontFamilyMeta>;

const cache = new Map<string, FontAtlas>();

export function font(
  family: FontFamilyId,
  weight: FontWeight,
  size: number,
): FontAtlas {
  const key = `${family}-${atlasKey(weight, size)}`;
  const cached = cache.get(key);
  if (cached) return cached;

  const file = FILES[family];
  if (!file) {
    throw new Error(`No glyph atlas for family "${family}".`);
  }
  const entry = file.atlases[atlasKey(weight, size)];
  if (entry === undefined) {
    throw new Error(
      `No glyph atlas for ${family} ${weight} ${size}. Add the size to FONT_SIZES in src/core/font.ts and regenerate with tools/gen_font_atlas.py.`,
    );
  }
  const parsed = FontAtlasSchema.parse(entry);
  cache.set(key, parsed);
  return parsed;
}

export function availableAtlases(): string[] {
  return Object.entries(FILES)
    .flatMap(([family, file]) =>
      Object.keys(file.atlases).map((key) => `${family}-${key}`),
    )
    .sort();
}
