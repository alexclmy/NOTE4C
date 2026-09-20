import { z } from "zod";
import { BLACK } from "@/core/palette";
import { OCTOPUS_DRAWN_H, OCTOPUS_DRAWN_W, drawOctopus } from "../sprites";
import type { WeatherValue } from "../data";
import { drawText, filled, reservedHeight, textElementSchema } from "../text";
import type { ModuleDefinition } from "../types";
import { clearModule, inner } from "./common";

/**
 * The mascot.
 *
 * The caption used to print the raw condition key ("partlycloudy") at a fixed
 * 10 px. It is now an editable, hideable line with {condition} and {temp}
 * placeholders, off by default, so the words beside the octopus are the owner's
 * choice and the values behind them stay the weather source's.
 */
export const OctopusOptions = z.object({
  /** Centre the mascot in its box rather than pinning it top-left. */
  centre: z.boolean().default(true),
  caption: textElementSchema({
    text: "{condition}",
    maxLength: 40,
    visible: false,
    style: { size: 13, align: "center" },
  }).default({}),
});
export type OctopusOptions = z.infer<typeof OctopusOptions>;

export const octopus: ModuleDefinition<OctopusOptions, WeatherValue> = {
  type: "octopus",
  label: "Weather octopus",
  description:
    "The mascot, with an accessory chosen from the current condition. Pixel art at native scale: no assets, no network, no model calls in any render path.",
  schema: OctopusOptions,
  defaultOptions: OctopusOptions.parse({}),
  defaultSpan: { w: 2, h: 2 },
  minSpan: { w: 2, h: 2 },
  maxSpan: { w: 3, h: 3 },
  sourceBinding: "weather",

  migrateOptions(legacy) {
    return {
      ...(typeof legacy.centre === "boolean" ? { centre: legacy.centre } : {}),
      ...(typeof legacy.showCaption === "boolean"
        ? { caption: { visible: legacy.showCaption } }
        : {}),
    };
  },

  render(fb, rect, data, options, ctx) {
    clearModule(fb, rect);
    const box = inner(rect);
    const reporter = ctx.report;

    // The mascot has no unavailable state of its own: an unknown condition is
    // simply the plain octopus with a cloud, and the weather module beside it
    // carries the honest "indisponible" wording.
    const condition =
      data.state === "ok" && data.value ? data.value.condition : "unknown";
    const temp =
      data.state === "ok" && data.value ? data.value.slots[0]?.temp : undefined;

    const caption = filled(options.caption, {
      condition,
      temp: temp ?? "—",
    });
    const captionHeight = reservedHeight(caption);

    const x = options.centre
      ? box.x + Math.max(0, Math.floor((box.w - OCTOPUS_DRAWN_W) / 2))
      : box.x;
    const y = options.centre
      ? box.y +
        Math.max(0, Math.floor((box.h - captionHeight - OCTOPUS_DRAWN_H) / 2))
      : box.y;

    drawOctopus(fb, x, y, {
      condition,
      ...(temp !== undefined ? { temp } : {}),
    });

    if (captionHeight > 0) {
      const captionY = y + OCTOPUS_DRAWN_H + 2;
      drawText(
        fb,
        { x: box.x, y: captionY, w: box.w, h: box.y + box.h - captionY },
        caption,
        BLACK,
        "caption",
        reporter ? { reporter } : {},
      );
    }
  },
};
