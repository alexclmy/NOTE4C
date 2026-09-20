/**
 * Everything this tower learns about its own machine.
 *
 * These tests exist because of what publishing this project means: the paths,
 * the address, the calendar name and the coordinates that used to be written
 * into the source were one household's. Now every one of them is an
 * environment variable, and the property that has to hold is the boring one —
 * **with nothing set, nothing is assumed.** A source with no configuration
 * reports `not configured` and does not reach for a file that is not there,
 * does not spawn a binary that does not exist, and does not fetch a forecast
 * for a city nobody chose.
 */

import { afterEach, beforeEach, describe, expect, it } from "vitest";
import {
  bridgeTokenPath,
  calendarName,
  calendarSnapshotPath,
  composerOrigin,
  defaultDeviceAddress,
  homeAssistantEnvPath,
  panelTimeZone,
  remindctlBin,
  weatherLocation,
} from "@/server/config";

const KEYS = [
  "NOTE4C_BRIDGE_TOKEN_PATH",
  "NOTE4C_CALENDAR_SNAPSHOT_PATH",
  "NOTE4C_CALENDAR_NAME",
  "NOTE4C_REMINDCTL_BIN",
  "NOTE4C_HA_ENV_PATH",
  "NOTE4C_COMPOSER_ORIGIN",
  "NOTE4C_WEATHER_LATITUDE",
  "NOTE4C_WEATHER_LONGITUDE",
  "NOTE4C_WEATHER_LABEL",
  "NOTE4C_PANEL_TIMEZONE",
  "NOTE4C_DEVICE_ADDRESS",
] as const;

/**
 * A clean environment on both sides of every test here.
 *
 * The suite as a whole pins NOTE4C_PANEL_TIMEZONE (see vitest.config.mts), and
 * these tests are precisely the ones about what happens when nothing is set —
 * so they clear the variables first rather than inheriting the harness's.
 */
beforeEach(() => {
  for (const key of KEYS) delete process.env[key];
});

afterEach(() => {
  for (const key of KEYS) delete process.env[key];
});

describe("with nothing configured", () => {
  it("has no bridge token path to offer", () => {
    expect(bridgeTokenPath()).toBe("");
  });

  it("has no calendar snapshot to read", () => {
    expect(calendarSnapshotPath()).toBe("");
  });

  it("does not demand a particular calendar name", () => {
    // Null means "whatever the snapshot says it is". The old literal was one
    // household's calendar, and a schema that insisted on it would reject
    // every other installation's snapshot as malformed.
    expect(calendarName()).toBeNull();
  });

  it("has no reminders binary", () => {
    expect(remindctlBin()).toBe("");
  });

  it("has no Home Assistant environment file", () => {
    expect(homeAssistantEnvPath()).toBe("");
  });

  it("has no second composer to read", () => {
    expect(composerOrigin()).toBe("");
  });

  it("has no coordinates, rather than somebody else's city", () => {
    expect(weatherLocation()).toBeNull();
  });

  it("falls back to the machine's own timezone for the panel", () => {
    const system = Intl.DateTimeFormat().resolvedOptions().timeZone;
    expect(panelTimeZone()).toBe(system);
  });

  it("has no device address, so the tower asks for one", () => {
    expect(defaultDeviceAddress()).toBe("");
  });
});

describe("with the environment set", () => {
  it("takes each path verbatim", () => {
    process.env.NOTE4C_BRIDGE_TOKEN_PATH = "/tmp/token";
    process.env.NOTE4C_CALENDAR_SNAPSHOT_PATH = "/tmp/snapshot.json";
    process.env.NOTE4C_REMINDCTL_BIN = "/usr/local/bin/remindctl";
    process.env.NOTE4C_HA_ENV_PATH = "/tmp/ha.env";
    expect(bridgeTokenPath()).toBe("/tmp/token");
    expect(calendarSnapshotPath()).toBe("/tmp/snapshot.json");
    expect(remindctlBin()).toBe("/usr/local/bin/remindctl");
    expect(homeAssistantEnvPath()).toBe("/tmp/ha.env");
  });

  it("reads the calendar name", () => {
    process.env.NOTE4C_CALENDAR_NAME = "Household";
    expect(calendarName()).toBe("Household");
  });

  it("reads the composer origin", () => {
    process.env.NOTE4C_COMPOSER_ORIGIN = "http://127.0.0.1:9731";
    expect(composerOrigin()).toBe("http://127.0.0.1:9731");
  });

  it("reads coordinates, with a label when one is given", () => {
    process.env.NOTE4C_WEATHER_LATITUDE = "48.8566";
    process.env.NOTE4C_WEATHER_LONGITUDE = "2.3522";
    process.env.NOTE4C_WEATHER_LABEL = "Paris approx.";
    expect(weatherLocation()).toEqual({
      latitude: 48.8566,
      longitude: 2.3522,
      label: "Paris approx.",
    });
  });

  it("labels a configured location even when no label was given", () => {
    process.env.NOTE4C_WEATHER_LATITUDE = "48.8566";
    process.env.NOTE4C_WEATHER_LONGITUDE = "2.3522";
    // The panel has to say where a number came from. A coordinate pair is a
    // worse label than a place name but an honest one, and better than a
    // temperature with no provenance at all.
    expect(weatherLocation()?.label).toContain("48.86");
  });

  it("refuses half a location rather than guessing the other half", () => {
    process.env.NOTE4C_WEATHER_LATITUDE = "48.8566";
    expect(weatherLocation()).toBeNull();
  });

  it("refuses coordinates that are not numbers, or not on the planet", () => {
    process.env.NOTE4C_WEATHER_LATITUDE = "north";
    process.env.NOTE4C_WEATHER_LONGITUDE = "2.3522";
    expect(weatherLocation()).toBeNull();

    process.env.NOTE4C_WEATHER_LATITUDE = "91";
    expect(weatherLocation()).toBeNull();
  });

  it("takes the panel timezone", () => {
    process.env.NOTE4C_PANEL_TIMEZONE = "Europe/Paris";
    expect(panelTimeZone()).toBe("Europe/Paris");
  });

  it("ignores a timezone this machine cannot resolve", () => {
    // A bad IANA name would throw inside Intl on every single render, which
    // on the panel means a blank tile rather than a wrong time.
    process.env.NOTE4C_PANEL_TIMEZONE = "Mars/Olympus";
    expect(panelTimeZone()).toBe(Intl.DateTimeFormat().resolvedOptions().timeZone);
  });

  it("takes a device address", () => {
    process.env.NOTE4C_DEVICE_ADDRESS = "192.0.2.60";
    expect(defaultDeviceAddress()).toBe("192.0.2.60");
  });

  it("treats whitespace as absent", () => {
    process.env.NOTE4C_COMPOSER_ORIGIN = "   ";
    expect(composerOrigin()).toBe("");
  });
});
