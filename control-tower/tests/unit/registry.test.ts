import { describe, expect, it } from "vitest";
import {
  APPLY_MODE_BADGE,
  APPLY_MODE_COPY,
  CAP_CONFIG_V2,
  CAP_VOICE_PTT_V1,
  SETTINGS_REGISTRY,
  aboutFields,
  entryByKey,
  isEditable,
  isGated,
  isLive,
  needsConfirmation,
  negotiate,
  writableFields,
} from "@/core/registry";
import { DEFAULT_CAPABILITIES } from "@mock/server";

/**
 * The registry is where the Device page's honesty lives.
 *
 * Every claim on that page is a function of this table and the device's own
 * capability list, so these tests are the ones that would catch a control
 * rendering for a firmware that cannot serve it.
 */

const API_1 = negotiate({ api: 1 });
const API_2 = negotiate({ api: 2, capabilities: [...DEFAULT_CAPABILITIES] });
const API_2_NO_CONFIG = negotiate({
  api: 2,
  capabilities: ["dashboard.frame.v1", "voice.hub.v1"],
});

describe("negotiation", () => {
  it("defaults to api 1 with no capabilities when the device says nothing", () => {
    expect(negotiate({})).toEqual({ api: 1, capabilities: [] });
  });

  it("reads the api level and the capability list off a status response", () => {
    expect(API_2.api).toBe(2);
    expect(API_2.capabilities).toContain(CAP_CONFIG_V2);
  });
});

describe("capability gating", () => {
  it("renders nothing editable against an api 1 device", () => {
    const editable = SETTINGS_REGISTRY.filter((entry) => isEditable(entry, API_1));
    // At api 1 the only remotely writable setting is the hub, and it goes
    // through the v1 route.
    expect(editable.map((entry) => entry.key)).toEqual(["voice.hub"]);
  });

  it("lights up the typed fields once the device reports config.v2", () => {
    const fields = writableFields(API_2).map((entry) => entry.field);
    expect(fields).toEqual([
      "sync.sync_interval",
      "gallery.slide_min",
      "network.lan_service",
      "dashboard.lockdown",
      "voice.muted",
    ]);
  });

  it("keeps them dark on an api 2 build that omitted the capability", () => {
    // This is the case a plain api integer cannot express: the contract is v2,
    // but this build did not compile the config API.
    expect(writableFields(API_2_NO_CONFIG)).toEqual([]);
    const slideshow = entryByKey("gallery.slide_min")!;
    expect(isLive(slideshow, API_2_NO_CONFIG)).toBe(false);
    expect(isGated(slideshow, API_2_NO_CONFIG)).toBe(true);
  });

  it("does not call a read-only row gated, because it is not on the device", () => {
    const address = entryByKey("network.lan_address")!;
    expect(isGated(address, API_1)).toBe(false);
    expect(isGated(address, API_2)).toBe(false);
  });

  it("keeps Wi-Fi credentials out of reach at every api level", () => {
    const wifi = entryByKey("network.wifi")!;
    for (const device of [API_1, API_2, API_2_NO_CONFIG]) {
      expect(isEditable(wifi, device)).toBe(false);
      expect(isLive(wifi, device)).toBe(false);
    }
  });

  it("has no registry row that could write a credential", () => {
    for (const entry of SETTINGS_REGISTRY) {
      expect(entry.field ?? "").not.toMatch(/token|password|secret/i);
    }
  });

  it("gates the actions on their own capabilities", () => {
    const restart = entryByKey("system.restart")!;
    const sleep = entryByKey("system.sleep")!;
    expect(isLive(restart, API_2)).toBe(true);
    expect(isLive(sleep, API_2)).toBe(true);
    expect(isLive(restart, API_1)).toBe(false);
    expect(isLive(restart, negotiate({ api: 2, capabilities: ["config.v2"] }))).toBe(
      false,
    );
  });
});

describe("confirmation gates", () => {
  it("asks for a word only in the direction that severs the connection", () => {
    const lan = entryByKey("network.lan_service")!;
    expect(needsConfirmation(lan, false)).toBe(true);
    expect(needsConfirmation(lan, true)).toBe(false);
  });

  it("pairs the word the user types with the literal the device requires", () => {
    const lan = entryByKey("network.lan_service")!;
    expect(lan.confirmGate).toBe("LAN OFF");
    expect(lan.deviceConfirm).toBe("lan_service_off");

    const restart = entryByKey("system.restart")!;
    expect(restart.confirmGate).toBe("RESTART");
    expect(restart.deviceConfirm).toBe("restart");
  });

  it("asks for nothing on a setting that cannot strand anybody", () => {
    expect(needsConfirmation(entryByKey("voice.muted")!, false)).toBe(false);
    expect(needsConfirmation(entryByKey("gallery.slide_min")!, 30)).toBe(false);
  });
});

describe("one-way lockdown", () => {
  it("is declared one way in the table, not only enforced on the device", () => {
    const lockdown = entryByKey("dashboard.lockdown")!;
    expect(lockdown.oneWayTo).toBe(true);
  });
});

describe("apply mode copy", () => {
  it("has a badge and a sentence for each of the three modes", () => {
    for (const mode of [
      "immediate",
      "immediate_not_persisted",
      "restart_required",
    ] as const) {
      expect(APPLY_MODE_BADGE[mode]).toBeTruthy();
      expect(APPLY_MODE_COPY[mode]).toBeTruthy();
    }
  });

  it("says plainly that a restart is needed rather than implying it", () => {
    expect(APPLY_MODE_COPY.restart_required).toContain("restart");
    expect(APPLY_MODE_BADGE.restart_required).toBe("restart required");
  });

  it("does not call the LAN service setting persisted, because it is not", () => {
    const lan = entryByKey("network.lan_service")!;
    expect(lan.applyMode).toBe("immediate_not_persisted");
    expect(APPLY_MODE_COPY.immediate_not_persisted).toContain("default after a restart");
  });

  it("has no copy string containing an em dash", () => {
    for (const value of [
      ...Object.values(APPLY_MODE_COPY),
      ...Object.values(APPLY_MODE_BADGE),
      ...SETTINGS_REGISTRY.flatMap((entry) => [entry.note ?? "", entry.evidence]),
    ]) {
      expect(value).not.toContain("—");
    }
  });
});

describe("About fields", () => {
  it("reads the product name off the device instead of inventing one", () => {
    const fields = aboutFields({
      firmware: "NOTE4C-mock 0.2",
      api: 2,
      capabilities: [...DEFAULT_CAPABILITIES],
      device: {
        name: "NOTE4C mock panel",
        model: "zectrix-s3-epaper-4.2",
        hardware: "NOTE4C 4-color",
        fw: "NOTE4C-mock 0.2",
        upstream_base: "6.5.9",
      },
      config_revision: 4,
    });
    const byKey = Object.fromEntries(fields.map((field) => [field.key, field.value]));
    expect(byKey.device_name).toBe("NOTE4C mock panel");
    expect(byKey.firmware).toBe("NOTE4C-mock 0.2");
    expect(byKey.hardware).toBe("NOTE4C 4-color");
    expect(byKey.config_revision).toBe("4");
    expect(byKey.upstream_base).toBe("6.5.9");
  });

  it("says a value was not reported rather than substituting a vendor name", () => {
    const fields = aboutFields({ firmware: "6.5.9", api: 1 });
    const byKey = Object.fromEntries(fields.map((field) => [field.key, field.value]));
    expect(byKey.device_name).toBe("not reported");
    expect(byKey.hardware).toBe("not reported");
    // The two inherited vendor strings the tower used to print are gone.
    const serialised = JSON.stringify(fields);
    expect(serialised).not.toContain("notellm");
    expect(serialised).not.toContain("Youn-Beta1.0");
    expect(serialised).not.toContain("lazyyoun");
  });

  it("omits the revision row entirely when the device does not report one", () => {
    const fields = aboutFields({ firmware: "6.5.9", api: 1 });
    expect(fields.find((field) => field.key === "config_revision")).toBeUndefined();
  });

  it("gives every field a provenance tooltip", () => {
    for (const field of aboutFields({ firmware: "x", api: 2, capabilities: [] })) {
      expect(field.tooltip.length).toBeGreaterThan(20);
    }
  });
});

describe("push to talk, absent versus muted", () => {
  it("is reported as a capability, so absent and muted are distinguishable", () => {
    const withPtt = negotiate({
      api: 2,
      capabilities: [...DEFAULT_CAPABILITIES, CAP_VOICE_PTT_V1],
    });
    expect(withPtt.capabilities).toContain(CAP_VOICE_PTT_V1);
    expect(API_2.capabilities).not.toContain(CAP_VOICE_PTT_V1);
  });
});
