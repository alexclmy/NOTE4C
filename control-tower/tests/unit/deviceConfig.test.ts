import { afterEach, beforeEach, describe, expect, it } from "vitest";
import { MockDevice } from "@mock/server";
import {
  DeviceClient,
  DeviceError,
  DeviceRevisionConflict,
} from "@/server/device/client";

/**
 * The api 2 configuration contract, driven through the real client against the
 * faithful mock.
 *
 * What is being checked here is almost entirely refusals. A config API that
 * accepts what it should is easy; one that refuses an unknown field, a stale
 * revision, a lockdown somebody tries to lower from the network, and a reboot
 * that arrives twice is the part worth testing before anything is flashed.
 */

const TOKEN = "d".repeat(64);
const HUB_TOKEN = "hub-token-0123456789abcdef";

let device: MockDevice;
let client: DeviceClient;

beforeEach(async () => {
  device = new MockDevice({ token: TOKEN, panelDelayMs: 10 });
  const origin = await device.listen(0);
  client = new DeviceClient({
    mode: "mock",
    address: "192.168.7.7",
    mockOrigin: origin,
    token: TOKEN,
  });
});

afterEach(async () => {
  await device.close();
});

describe("reading the configuration", () => {
  it("returns every allowlisted field with its revision", async () => {
    const config = await client.getConfig();
    expect(config.api).toBe(2);
    expect(config.revision).toBe(0);
    expect(config.config.gallery.slide_min).toBe(5);
    expect(config.config.sync.sync_interval).toBe(30);
    expect(config.config.voice.muted).toBe(true);
    expect(config.config.dashboard.lockdown).toBe(true);
    expect(config.config.network.lan_service).toBe(true);
  });

  it("states that Wi-Fi is not writable rather than leaving it out", async () => {
    const config = await client.getConfig();
    expect(config.config.network.wifi_writable).toBe(false);
  });

  it("requires the device token", async () => {
    const anonymous = new DeviceClient({
      mode: "mock",
      address: "192.168.7.7",
      mockOrigin: device.origin,
      token: "e".repeat(64),
    });
    await expect(anonymous.getConfig()).rejects.toThrow(DeviceError);
  });

  it("shares the frame route's failure lockout", async () => {
    const wrong = new DeviceClient({
      mode: "mock",
      address: "192.168.7.7",
      mockOrigin: device.origin,
      token: "f".repeat(64),
    });
    for (let i = 0; i < 10; i += 1) {
      await wrong.getConfig().catch(() => undefined);
    }
    // The eleventh attempt is locked out, and so is a correct token: the
    // config routes cannot be used to probe more cheaply than a frame push.
    await expect(client.getConfig()).rejects.toMatchObject({ status: 429 });
  });
});

describe("secret non-disclosure", () => {
  it("reports the hub token as a boolean and never as a value", async () => {
    await client.setVoiceHub("http://192.168.7.7:8770", HUB_TOKEN);
    const config = await client.getConfig();

    expect(config.config.voice.hub_token_set).toBe(true);
    expect(config.config.voice.hub_url).toBe("http://192.168.7.7:8770");

    // The strongest form available here: search the whole serialised response
    // for the exact secret that was just written.
    const serialised = JSON.stringify(config);
    expect(serialised).not.toContain(HUB_TOKEN);
    expect(serialised).not.toContain(TOKEN);
    expect(serialised).not.toMatch(/"hub_token"\s*:/);
  });

  it("does not echo the token back from the write route either", async () => {
    const result = await client.setVoiceHub("http://192.168.7.7:8770", HUB_TOKEN);
    expect(result.tokenSet).toBe(true);
    expect(JSON.stringify(result)).not.toContain(HUB_TOKEN);
  });

  it("moves the revision when the hub write changes a reported setting", async () => {
    const before = (await client.getConfig()).revision;
    await client.setVoiceHub("http://192.168.7.7:8770", HUB_TOKEN);
    const after = (await client.getConfig()).revision;
    expect(after).toBeGreaterThan(before);
  });
});

describe("writing the configuration", () => {
  it("applies every field of a valid patch and reports how each took effect", async () => {
    const before = await client.getConfig();
    const result = await client.patchConfig(before.revision, {
      "gallery.slide_min": 30,
      "voice.muted": false,
      "sync.sync_interval": 0,
    });

    expect(result.revision).toBe(before.revision + 1);
    expect(result.config.gallery.slide_min).toBe(30);
    expect(result.config.voice.muted).toBe(false);
    expect(result.config.sync.sync_interval).toBe(0);
    expect(result.applied).toMatchObject({
      "gallery.slide_min": "immediate",
      "voice.muted": "immediate",
      "sync.sync_interval": "immediate",
    });

    // And a fresh read agrees, so the response was not merely optimistic.
    const after = await client.getConfig();
    expect(after.config.gallery.slide_min).toBe(30);
    expect(after.revision).toBe(result.revision);
  });

  it("says when a field will not survive a restart, rather than calling it immediate", async () => {
    const before = await client.getConfig();
    const result = await client.patchConfig(
      before.revision,
      { "network.lan_service": false },
      "lan_service_off",
    );
    expect(result.applied?.["network.lan_service"]).toBe("immediate_not_persisted");
  });
});

describe("validation", () => {
  it("refuses an unknown field and names it", async () => {
    const { revision } = await client.getConfig();
    await expect(
      client.patchConfig(revision, { "gallery.slide_minutes": 5 }),
    ).rejects.toMatchObject({ code: "unknown_field" });
  });

  it("refuses a value the device menu itself cannot produce", async () => {
    const { revision } = await client.getConfig();
    await expect(
      client.patchConfig(revision, { "gallery.slide_min": 7 }),
    ).rejects.toMatchObject({ code: "out_of_range" });
    await expect(
      client.patchConfig(revision, { "sync.sync_interval": 1441 }),
    ).rejects.toMatchObject({ code: "out_of_range" });
  });

  it("refuses a boolean sent as a number", async () => {
    const { revision } = await client.getConfig();
    await expect(
      client.patchConfig(revision, { "voice.muted": 1 }),
    ).rejects.toMatchObject({ code: "wrong_type" });
  });

  it("refuses a hub URL with no token behind it", async () => {
    const { revision } = await client.getConfig();
    await expect(
      client.patchConfig(revision, { "voice.hub_url": "http://192.168.7.8:8770" }),
    ).rejects.toMatchObject({ code: "hub_url_needs_token" });
  });

  it("refuses a hub URL that would forge a request", async () => {
    await client.setVoiceHub("http://192.168.7.7:8770", HUB_TOKEN);
    const { revision } = await client.getConfig();
    await expect(
      client.patchConfig(revision, { "voice.hub_url": "ftp://192.168.7.8" }),
    ).rejects.toMatchObject({ code: "bad_hub_url" });
  });

  it("applies none of a patch that fails on any field", async () => {
    const before = await client.getConfig();
    await expect(
      client.patchConfig(before.revision, {
        "gallery.slide_min": 30,
        "sync.sync_interval": 99999,
      }),
    ).rejects.toThrow();

    const after = await client.getConfig();
    expect(after.config.gallery.slide_min).toBe(5);
    // The revision did not move either, so a caller that re-reads sees exactly
    // what it saw before it tried.
    expect(after.revision).toBe(before.revision);
  });

  it("needs the literal before it will sever its own connection", async () => {
    const { revision } = await client.getConfig();
    await expect(
      client.patchConfig(revision, { "network.lan_service": false }),
    ).rejects.toMatchObject({ code: "missing_confirmation" });
    await expect(
      client.patchConfig(revision, { "network.lan_service": false }, "yes"),
    ).rejects.toMatchObject({ code: "bad_confirmation" });

    const ok = await client.patchConfig(
      revision,
      { "network.lan_service": false },
      "lan_service_off",
    );
    expect(ok.config.network.lan_service).toBe(false);
  });
});

describe("the one-way lockdown", () => {
  it("lets the tower block the legacy write routes", async () => {
    device.bumpRevisionLocally((config) => {
      config.dashboard.lockdown = false;
    });
    const { revision } = await client.getConfig();
    const result = await client.patchConfig(revision, { "dashboard.lockdown": true });
    expect(result.config.dashboard.lockdown).toBe(true);
  });

  it("never lets it unblock them, whichever state they are in", async () => {
    for (const startingState of [true, false]) {
      device.bumpRevisionLocally((config) => {
        config.dashboard.lockdown = startingState;
      });
      const { revision } = await client.getConfig();
      await expect(
        client.patchConfig(revision, { "dashboard.lockdown": false }),
      ).rejects.toMatchObject({ status: 403 });
      // And nothing changed as a result of asking.
      expect((await client.getConfig()).config.dashboard.lockdown).toBe(startingState);
    }
  });
});

describe("compare and swap", () => {
  it("refuses a write whose revision is stale and says what the device is at", async () => {
    const stale = await client.getConfig();
    // Somebody presses buttons on the device between the read and the write.
    device.bumpRevisionLocally((config) => {
      config.gallery.slide_min = 10;
    });

    const failure = await client
      .patchConfig(stale.revision, { "gallery.slide_min": 30 })
      .catch((error: unknown) => error);

    expect(failure).toBeInstanceOf(DeviceRevisionConflict);
    expect((failure as DeviceRevisionConflict).deviceRevision).toBe(
      stale.revision + 1,
    );
  });

  it("leaves the physical change in place rather than overwriting it", async () => {
    const stale = await client.getConfig();
    device.bumpRevisionLocally((config) => {
      config.gallery.slide_min = 10;
    });
    await client
      .patchConfig(stale.revision, { "gallery.slide_min": 30 })
      .catch(() => undefined);

    // This is the whole point of the CAS: the value somebody set by hand is
    // still the value.
    expect((await client.getConfig()).config.gallery.slide_min).toBe(10);
  });

  it("succeeds once the caller has re-read", async () => {
    const stale = await client.getConfig();
    device.bumpRevisionLocally();
    await client.patchConfig(stale.revision, { "voice.muted": false }).catch(() => undefined);

    const fresh = await client.getConfig();
    const result = await client.patchConfig(fresh.revision, { "voice.muted": false });
    expect(result.config.voice.muted).toBe(false);
  });
});

describe("actions", () => {
  it("schedules a restart after a delay rather than immediately", async () => {
    const result = await client.runAction("restart", "key-1");
    expect(result.scheduled).toBe(true);
    expect(result.replay).toBe(false);
    // Not zero: the response has to reach the socket before the device stops
    // being a device.
    expect(result.atMs).toBeGreaterThanOrEqual(1000);
    expect(device.performedActions).toEqual([{ action: "restart", atMs: 1000 }]);
  });

  it("answers a retry with the same key without acting twice", async () => {
    await client.runAction("restart", "key-same");
    const second = await client.runAction("restart", "key-same");

    expect(second.replay).toBe(true);
    expect(second.scheduled).toBe(false);
    // Once. A caller that retried after a timeout does not reboot the device
    // a second time.
    expect(device.performedActions).toHaveLength(1);
  });

  it("treats a different key as a different intent", async () => {
    await client.runAction("restart", "key-1");
    await client.runAction("sleep", "key-2");
    expect(device.performedActions.map((entry) => entry.action)).toEqual([
      "restart",
      "sleep",
    ]);
  });

  it("refuses an action with no idempotency key", async () => {
    await expect(client.runAction("restart", "")).rejects.toMatchObject({
      code: "missing_idempotency_key",
    });
    expect(device.performedActions).toHaveLength(0);
  });

  it("refuses an unauthenticated action", async () => {
    const anonymous = new DeviceClient({
      mode: "mock",
      address: "192.168.7.7",
      mockOrigin: device.origin,
      token: "0".repeat(64),
    });
    await expect(anonymous.runAction("restart", "key-1")).rejects.toThrow(DeviceError);
    expect(device.performedActions).toHaveLength(0);
  });
});

describe("an api 1 device", () => {
  it("has no config route at all, and the tower finds that out as a 404", async () => {
    const old = new MockDevice({ token: TOKEN, api: 1 });
    const oldOrigin = await old.listen(0);
    const oldClient = new DeviceClient({
      mode: "mock",
      address: "192.168.7.7",
      mockOrigin: oldOrigin,
      token: TOKEN,
    });
    try {
      await expect(oldClient.getConfig()).rejects.toThrow(DeviceError);
      await expect(oldClient.runAction("restart", "key-1")).rejects.toThrow(
        DeviceError,
      );
    } finally {
      await old.close();
    }
  });
});
