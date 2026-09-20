import localFont from "next/font/local";

/**
 * The two faces the interface actually claims, served from this repository.
 *
 * Two rules hold here and are worth stating because they are easy to break:
 *
 *  - **Nothing is fetched at runtime.** `next/font/local` reads the TTFs at
 *    build time and self-hosts them. No Google Fonts call, no CDN, no third
 *    party learning who opens this tower. That is the same promise the glyph
 *    atlases make on the panel side, and `tools/screenshots.ts` fails the QA
 *    run on any external request, so it is checked rather than intended.
 *  - **Both faces are OFL 1.1**, with the licence text vendored next to them
 *    (`assets/fonts/<family>/OFL.txt`), so redistributing them inside this
 *    repository is exactly what their licence permits. The Diagnostics page
 *    shows the provenance; `NOTICE` states it for the distribution.
 *
 * Space Grotesk sets *everything* — titles and body both. That is a change
 * from the first version of this file, where `--font-body` was the system
 * stack on the reasoning that the reader's eye is already calibrated to it.
 * The interface this repository now draws is a designed object rather than a
 * neutral one: grotesque capitals, tight tracking, a 2 px rule around
 * everything. The system face fought it on every screen. One shipped variable
 * file covering 300-700 costs less than the two static cuts it replaces, so
 * the weight bought is smaller than the weight that was being avoided.
 *
 * Poppins stays vendored even though nothing in the browser asks for it any
 * more: `src/core/render/fonts/poppins.json` was rasterised from it, and a
 * dashboard may still select that face for the panel.
 */

export const titleFont = localFont({
  src: [
    {
      // One variable file, weights 300-700. Space Grotesk ships no static
      // SemiBold, and the prototype leans on 500 and 600 for chip titles and
      // section headings — synthesising them from Regular and Bold would have
      // smeared exactly the weights the design uses most.
      path: "../../assets/fonts/spacegrotesk/SpaceGrotesk-Variable.ttf",
      weight: "300 700",
      style: "normal",
    },
  ],
  variable: "--font-title-local",
  display: "swap",
  fallback: ["-apple-system", "BlinkMacSystemFont", "Segoe UI", "sans-serif"],
  adjustFontFallback: false,
});

export const monoFont = localFont({
  src: [
    { path: "../../assets/fonts/plexmono/IBMPlexMono_400Regular.ttf", weight: "400", style: "normal" },
    // 500 and 600 are not decoration. Every meta label in this interface is
    // spaced mono capitals at one of those two weights, and letting the
    // browser round them to 400 or 700 changed the texture of every card.
    { path: "../../assets/fonts/plexmono/IBMPlexMono_500Medium.ttf", weight: "500", style: "normal" },
    { path: "../../assets/fonts/plexmono/IBMPlexMono_600SemiBold.ttf", weight: "600", style: "normal" },
    { path: "../../assets/fonts/plexmono/IBMPlexMono_700Bold.ttf", weight: "700", style: "normal" },
  ],
  variable: "--font-mono-local",
  display: "swap",
  fallback: ["ui-monospace", "SFMono-Regular", "Menlo", "monospace"],
  adjustFontFallback: false,
});

/** Both variable classes, for the <html> element. */
export const fontVariables = `${titleFont.variable} ${monoFont.variable}`;
