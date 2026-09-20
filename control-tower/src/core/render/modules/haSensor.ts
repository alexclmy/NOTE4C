import { z } from "zod";
import { BLACK, RED } from "@/core/palette";
import type { SensorValue } from "../data";
import {
  drawText,
  reservedHeight,
  textElementSchema,
  textStyleSchema,
} from "../text";
import type { ModuleDefinition } from "../types";
import { clearModule, inner } from "./common";

/**
 * One read-only Home Assistant sensor.
 *
 * The label and both failure wordings are the owner's text. The reading is the
 * sensor's, and the stale marker is composed from an editable suffix rather
 * than welded to the number, so he can drop the word "périmé" without
 * dropping the fact that the value is old: stale still renders in the
 * attention colour whatever the suffix says.
 */
export const HaSensorOptions = z.object({
  /**
   * Read-only entity domains only. A dashboard tile must never be able to
   * name something the tower could actuate.
   */
  entityId: z
    .string()
    .regex(
      /^(sensor|binary_sensor)\.[a-z0-9_]+$/,
      "Only sensor. and binary_sensor. entities can be displayed",
    )
    .default("sensor.example_temperature"),
  /*
   * 11 px, which is small, and deliberately so: it is a caption over the
   * reading, not the reading. A one-row tile is 42 px inside its padding, and
   * an 18 px monospace value needs 24 of them. Raising this to 13 px is one
   * dropdown away and costs the value its room, which the overflow warning
   * will say out loud rather than silently swallowing the number.
   */
  label: textElementSchema({
    text: "Capteur",
    maxLength: 40,
    style: { size: 11 },
  }).default({}),
  /** Fixed pitch by default so a changing reading does not shift its own tile. */
  valueStyle: textStyleSchema({ family: "plexmono", size: 18 }).default({}),
  /** Appended to the reading when it is older than the freshness window. */
  staleSuffix: textElementSchema({
    text: " · périmé",
    maxLength: 24,
    style: { size: 13 },
  }).default({}),
  unavailableText: textElementSchema({
    text: "indisponible",
    maxLength: 40,
    style: { size: 15 },
  }).default({}),
});
export type HaSensorOptions = z.infer<typeof HaSensorOptions>;

export const haSensor: ModuleDefinition<HaSensorOptions, SensorValue> = {
  type: "haSensor",
  label: "Home Assistant sensor",
  description:
    "One read-only sensor or binary_sensor value. A reading older than three hours renders as périmé rather than as a current number.",
  schema: HaSensorOptions,
  defaultOptions: HaSensorOptions.parse({}),
  defaultSpan: { w: 3, h: 1 },
  minSpan: { w: 2, h: 1 },
  maxSpan: { w: 4, h: 2 },
  sourceBinding: "haSensor",

  migrateOptions(legacy) {
    return {
      ...(typeof legacy.entityId === "string"
        ? { entityId: legacy.entityId }
        : {}),
      ...(typeof legacy.label === "string"
        ? { label: { text: legacy.label } }
        : {}),
    };
  },

  render(fb, rect, data, options, ctx) {
    clearModule(fb, rect);
    const box = inner(rect);
    const reporter = ctx.report;
    const report = reporter ? { reporter } : {};

    const labelHeight = reservedHeight(options.label);
    drawText(fb, box, options.label, BLACK, "label", report);

    const valueBox = {
      x: box.x,
      y: box.y + labelHeight,
      w: box.w,
      h: Math.max(0, box.h - labelHeight),
    };

    if (data.state === "unavailable" || !data.value) {
      // "indisponible" is a configured sensor that did not answer. It renders
      // in the attention colour and never as a zero or a dash.
      drawText(
        fb,
        valueBox,
        options.unavailableText,
        RED,
        "unavailableText",
        report,
      );
      return;
    }

    const stale = data.state === "stale";
    const suffix =
      stale && options.staleSuffix.visible ? options.staleSuffix.text : "";

    drawText(
      fb,
      valueBox,
      {
        text: `${data.value.value}${suffix}`,
        visible: true,
        style: options.valueStyle,
      },
      // The colour, not the wording, is what carries "do not trust this yet".
      stale ? RED : BLACK,
      "value",
      report,
    );
  },
};
