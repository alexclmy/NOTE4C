import { z } from "zod";
import { BLACK, RED } from "@/core/palette";
import { drawText, textElementSchema } from "../text";
import { chromeNow, type ModuleDefinition } from "../types";
import { clearModule, inner } from "./common";

/**
 * A short note with an expiry.
 *
 * The note itself is the one module that was always pure user text. What is
 * new is that it now carries typography and wraps: at the old fixed 12 px on
 * one line, a note longer than the tile was cut off without saying so.
 */
export const MessageOptions = z.object({
  body: textElementSchema({
    text: "",
    maxLength: 240,
    style: { size: 15 },
  }).default({}),
  /**
   * ISO 8601 with offset. An expired message disappears from the panel
   * instead of sitting there stale: e-paper holds ink indefinitely, so an
   * undated note outlives its meaning by design.
   */
  expiresAt: z.string().datetime({ offset: true }).nullable().default(null),
  /** Shown in the attention colour when expiresAt cannot be read as a date. */
  invalidExpiryText: textElementSchema({
    text: "Message : date d'expiration invalide",
    maxLength: 60,
    style: { size: 13 },
  }).default({}),
});
export type MessageOptions = z.infer<typeof MessageOptions>;

export const message: ModuleDefinition<MessageOptions, never> = {
  type: "message",
  label: "Message",
  description:
    "A short note with an expiry. After the expiry the module renders nothing, because frozen ink outlives an undated message.",
  schema: MessageOptions,
  defaultOptions: MessageOptions.parse({}),
  defaultSpan: { w: 6, h: 1 },
  minSpan: { w: 2, h: 1 },
  maxSpan: { w: 8, h: 2 },
  sourceBinding: "none",

  migrateOptions(legacy) {
    return {
      ...(typeof legacy.text === "string" ? { body: { text: legacy.text } } : {}),
      ...(typeof legacy.expiresAt === "string" || legacy.expiresAt === null
        ? { expiresAt: legacy.expiresAt }
        : {}),
    };
  },

  render(fb, rect, _data, options, ctx) {
    clearModule(fb, rect);
    const box = inner(rect);
    const reporter = ctx.report;
    const report = reporter ? { reporter } : {};

    if (options.expiresAt !== null) {
      const expiry = new Date(options.expiresAt);
      if (Number.isNaN(expiry.getTime())) {
        drawText(
          fb,
          box,
          options.invalidExpiryText,
          RED,
          "invalidExpiryText",
          { wrap: true, ...report },
        );
        return;
      }
      // chromeNow so a semantic hash does not depend on when it was taken:
      // whether the note has expired is a fact about the clock, and the dedup
      // gate compares content.
      if (expiry.getTime() <= chromeNow(ctx).getTime()) return;
    }

    drawText(fb, box, options.body, BLACK, "body", { wrap: true, ...report });
  },
};
