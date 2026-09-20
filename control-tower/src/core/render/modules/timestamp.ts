import { z } from "zod";
import { BLACK } from "@/core/palette";
import { drawText, filled, reservedHeight, textElementSchema } from "../text";
import { chromeNow, type ModuleDefinition } from "../types";
import { formatPanelStamp } from "../time";
import { clearModule, inner } from "./common";

/**
 * When the panel was last painted.
 *
 * The wording is editable and the whole line can be hidden, but {stamp} is a
 * placeholder rather than a text box: the panel must never be able to claim a
 * refresh time that did not happen. Deleting the module is the honest way to
 * not show one, and it is one button away in the designer.
 */
export const TimestampOptions = z.object({
  label: textElementSchema({
    text: "MAJ {stamp}",
    maxLength: 40,
    // Fixed pitch so the line does not reflow every minute, and bottom-right
    // by default because this module is a footer wherever it is placed.
    style: { family: "plexmono", size: 13, align: "right" },
  }).default({}),
});
export type TimestampOptions = z.infer<typeof TimestampOptions>;

export const timestamp: ModuleDefinition<TimestampOptions, never> = {
  type: "timestamp",
  label: "Last updated",
  description:
    "Render time as MAJ dd/mm HH:MM in the panel timezone (NOTE4C_PANEL_TIMEZONE, or this machine's). {stamp} is filled by the renderer, so the panel can never state a refresh time that did not happen.",
  schema: TimestampOptions,
  defaultOptions: TimestampOptions.parse({}),
  /*
   * Three cells, not two. "MAJ 11/09 01:07" is 120 px at the default 13 px
   * monospace and two cells give it 92. It still FITS in two if the wording is
   * shortened to "{stamp}", and the overflow warning says so rather than the
   * panel quietly printing "MAJ 11/09…" and losing the time.
   */
  defaultSpan: { w: 3, h: 1 },
  minSpan: { w: 2, h: 1 },
  maxSpan: { w: 4, h: 1 },
  sourceBinding: "none",

  migrateOptions(legacy) {
    const prefix = typeof legacy.prefix === "string" ? legacy.prefix : "MAJ";
    const align = legacy.align === "left" ? "left" : "right";
    return {
      label: {
        text: `${prefix} {stamp}`.trim(),
        style: { family: "plexmono", size: 13, align },
      },
    };
  },

  render(fb, rect, _data, options, ctx) {
    clearModule(fb, rect);
    const box = inner(rect);
    const reporter = ctx.report;

    // chromeNow, not ctx.now: this line only states the clock, so it freezes
    // when the render is being hashed rather than shown. Two pushes that
    // differ by a minute are the same panel and must not cost a refresh.
    const line = filled(options.label, {
      stamp: formatPanelStamp(chromeNow(ctx), ctx.timeZone),
    });
    // Sit on the bottom of the box rather than the top, so the footer reads as
    // a footer at any tile height.
    const y = box.y + Math.max(0, box.h - reservedHeight(line));

    drawText(
      fb,
      { x: box.x, y, w: box.w, h: box.y + box.h - y },
      line,
      BLACK,
      "label",
      reporter ? { reporter } : {},
    );
  },
};
