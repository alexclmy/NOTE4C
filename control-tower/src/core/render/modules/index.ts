import type { ModuleDefinition } from "../types";
import { calendarNext } from "./calendarNext";
import { conditionalMessage } from "./conditionalMessage";
import { countdown } from "./countdown";
import { haSensor } from "./haSensor";
import { image } from "./image";
import { headline } from "./headline";
import { list } from "./list";
import { message } from "./message";
import { octopus } from "./octopus";
import { sky } from "./sky";
import { timestamp } from "./timestamp";
import { weather24h } from "./weather24h";
import { weatherHero } from "./weatherHero";
import { weatherWeek } from "./weatherWeek";

/**
 * The module registry. Adding a module here is the only step needed to make
 * it appear in the designer, in the schema-driven inspector, and in the
 * server renderer: the UI is generated from these definitions, never
 * hand-listed, so the three can never disagree.
 */
const definitions = [
  headline,
  sky,
  weather24h,
  weatherHero,
  weatherWeek,
  calendarNext,
  haSensor,
  message,
  list,
  countdown,
  conditionalMessage,
  timestamp,
  octopus,
  image,
] as const;

// eslint-disable-next-line @typescript-eslint/no-explicit-any
export const MODULES: Record<string, ModuleDefinition<any, any>> =
  Object.fromEntries(definitions.map((definition) => [definition.type, definition]));

export const MODULE_TYPES = definitions.map((definition) => definition.type);

export type ModuleType = (typeof definitions)[number]["type"];

// eslint-disable-next-line @typescript-eslint/no-explicit-any
export function moduleDefinition(type: string): ModuleDefinition<any, any> {
  const definition = MODULES[type];
  if (!definition) {
    throw new Error(`Unknown module type "${type}"`);
  }
  return definition;
}

export function isKnownModuleType(type: string): boolean {
  return Object.prototype.hasOwnProperty.call(MODULES, type);
}

export {
  calendarNext,
  conditionalMessage,
  countdown,
  haSensor,
  headline,
  image,
  list,
  message,
  octopus,
  sky,
  timestamp,
  weather24h,
  weatherHero,
};
