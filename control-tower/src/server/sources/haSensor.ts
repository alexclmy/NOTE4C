import fs from "node:fs";
import { z } from "zod";
import type { SensorValue } from "@/core/render/data";
import { clean } from "@/core/font";
import type { ModuleData } from "@/core/render/types";
import { homeAssistantEnvPath } from "@/server/config";
import { SourceError, fetchJson } from "./http";
import { notConfigured } from "./notConfigured";

/**
 * Home Assistant credentials, read per request from wherever the operator
 * keeps them. Nothing is copied into the tower's own store, nothing is logged,
 * and the token never crosses back over the HTTP boundary.
 *
 * Two ways in, and the process environment wins: `HASS_URL` and `HASS_TOKEN`
 * set directly, or a dotenv file named by NOTE4C_HA_ENV_PATH. The file form
 * exists because these credentials usually already live in one, and copying a
 * long-lived token into a second place is how a token outlives its rotation.
 */
export function haEnvPath(): string {
  return homeAssistantEnvPath();
}

export const STALE_AFTER_SECONDS = 3 * 60 * 60;

export const ENTITY_PATTERN = /^(sensor|binary_sensor)\.[a-z0-9_]+$/;

/** Minimal dotenv reader: KEY=VALUE, optional quotes, # comments, no expansion. */
export function parseDotenv(contents: string): Record<string, string> {
  const out: Record<string, string> = {};
  for (const rawLine of contents.split("\n")) {
    const line = rawLine.trim();
    if (line.length === 0 || line.startsWith("#")) continue;
    const withoutExport = line.startsWith("export ") ? line.slice(7).trim() : line;
    const eq = withoutExport.indexOf("=");
    if (eq <= 0) continue;
    const key = withoutExport.slice(0, eq).trim();
    let value = withoutExport.slice(eq + 1).trim();
    if (
      (value.startsWith('"') && value.endsWith('"') && value.length >= 2) ||
      (value.startsWith("'") && value.endsWith("'") && value.length >= 2)
    ) {
      value = value.slice(1, -1);
    }
    out[key] = value;
  }
  return out;
}

export interface HaCredentials {
  url: string;
  token: string;
}

export function readCredentials(envPath: string = haEnvPath()): HaCredentials {
  let env: Record<string, string> = {};
  if (envPath.length > 0) {
    try {
      env = parseDotenv(fs.readFileSync(envPath, "utf8"));
    } catch {
      env = {};
    }
  }
  // Process environment wins, matching the composer's precedence.
  const url = process.env.HASS_URL ?? env.HASS_URL;
  const token = process.env.HASS_TOKEN ?? env.HASS_TOKEN;
  if (!url || !token) {
    throw new SourceError(
      notConfigured(
        "Set HASS_URL and HASS_TOKEN, or point NOTE4C_HA_ENV_PATH at a file holding them.",
      ),
    );
  }
  return { url: url.replace(/\/+$/, ""), token };
}

/** Is Home Assistant configured at all? Never reveals the values. */
export function haConfigured(envPath: string = haEnvPath()): boolean {
  try {
    readCredentials(envPath);
    return true;
  } catch {
    return false;
  }
}

const StateSchema = z.object({
  entity_id: z.string(),
  state: z.string(),
  attributes: z.record(z.string(), z.unknown()).optional(),
  last_updated: z.string().optional(),
  last_reported: z.string().optional(),
});
export type HaState = z.infer<typeof StateSchema>;

export function assertReadOnlyEntity(entityId: string): string {
  if (!ENTITY_PATTERN.test(entityId)) {
    throw new SourceError(
      "Only sensor. and binary_sensor. entities can be displayed",
    );
  }
  return entityId;
}

/**
 * Format one reading the way the panel expects, including the composer's
 * French words for the states that are not a number.
 */
export function formatSensor(
  state: HaState,
  now: Date,
): { value: string; stale: boolean } {
  if (state.state === "unknown") return { value: "inconnu", stale: false };
  if (state.state === "unavailable") {
    return { value: "indisponible", stale: false };
  }

  const reported = state.last_reported ?? state.last_updated;
  if (!reported) return { value: "date inconnue", stale: false };

  const observed = new Date(reported);
  if (Number.isNaN(observed.getTime())) {
    return { value: "date inconnue", stale: false };
  }

  const ageSeconds = (now.getTime() - observed.getTime()) / 1000;
  if (ageSeconds > STALE_AFTER_SECONDS) {
    return { value: "périmé", stale: true };
  }

  const unit = state.attributes?.unit_of_measurement;
  const suffix = typeof unit === "string" && unit.length > 0 ? ` ${unit}` : "";
  return { value: clean(`${state.state}${suffix}`), stale: false };
}

export async function readSensor(
  entityId: string,
  label: string,
  now: Date = new Date(),
  options: { envPath?: string } = {},
): Promise<ModuleData<SensorValue>> {
  try {
    assertReadOnlyEntity(entityId);
    const { url, token } = readCredentials(options.envPath ?? haEnvPath());
    const payload = await fetchJson(
      `${url}/api/states/${encodeURIComponent(entityId)}`,
      {
        timeoutMs: 15_000,
        headers: { authorization: `Bearer ${token}` },
      },
    );
    const parsed = StateSchema.safeParse(payload);
    if (!parsed.success) throw new SourceError("Unexpected entity shape");

    const { value, stale } = formatSensor(parsed.data, now);
    const observedAt = parsed.data.last_reported ?? parsed.data.last_updated;
    return {
      state: stale ? "stale" : "ok",
      value: { label: clean(label || entityId), value },
      ...(observedAt ? { observedAt } : {}),
    };
  } catch (error) {
    return {
      state: "unavailable",
      detail: error instanceof SourceError ? error.message : "Sensor unavailable",
    };
  }
}

/** Read every distinct entity a dashboard binds to, in one pass. */
export async function readSensors(
  bindings: Array<{ entityId: string; label: string }>,
  now: Date = new Date(),
  options: { envPath?: string } = {},
): Promise<Record<string, ModuleData<SensorValue>>> {
  const unique = new Map<string, string>();
  for (const binding of bindings) {
    if (!unique.has(binding.entityId)) {
      unique.set(binding.entityId, binding.label);
    }
  }

  const results = await Promise.all(
    [...unique.entries()].map(async ([entityId, label]) => {
      return [entityId, await readSensor(entityId, label, now, options)] as const;
    }),
  );

  return Object.fromEntries(results);
}
