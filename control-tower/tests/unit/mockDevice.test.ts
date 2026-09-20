import { afterEach, beforeEach, describe, expect, it } from "vitest";
import { createHash } from "node:crypto";
import {
  AUTH_FAILURE_LIMIT,
  FRAME_BYTES,
  IDEMPOTENCY_RING_DEPTH,
  MockDevice,
} from "@mock/server";

let device: MockDevice;
let origin: string;

const TOKEN = "a".repeat(64);

function frame(fill: number): Uint8Array {
  return new Uint8Array(FRAME_BYTES).fill(fill);
}

function digest(bytes: Uint8Array): string {
  return createHash("sha256").update(bytes).digest("hex");
}

/** fetch's BodyInit type does not admit Uint8Array in this lib target. */
function asBody(bytes: Uint8Array): BodyInit {
  return bytes as unknown as BodyInit;
}

interface StatusBody {
  firmware: string;
  api: number;
  capabilities?: string[];
  device?: {
    name?: string;
    model?: string;
    hardware?: string;
    fw?: string;
    upstream_base?: string;
  };
  config_revision?: number;
  provisioned: boolean;
  lockdown: boolean;
  stored: { present: boolean; seq: number; sha256: string };
  displayed: { present: boolean; seq: number; sha256: string };
  refresh: {
    state: string;
    pending: boolean;
    renders: number;
    skipped: number;
    coalesced: number;
  };
  timing_ms: Record<string, number>;
  storage: Record<string, number>;
}

async function statusFrom(target: string): Promise<StatusBody> {
  const response = await fetch(`${target}/api/v1/dashboard/status`);
  return (await response.json()) as StatusBody;
}

async function putTo(
  target: string,
  bytes: Uint8Array,
): Promise<Response> {
  return fetch(`${target}/api/v1/dashboard/frame`, {
    method: "PUT",
    headers: {
      "x-auth-token": TOKEN,
      "content-length": String(bytes.length),
      "x-frame-sha256": digest(bytes),
    },
    body: asBody(bytes),
  });
}

async function put(
  bytes: Uint8Array,
  options: {
    token?: string;
    sha?: string | null;
    key?: string;
    contentLength?: number;
  } = {},
): Promise<{ status: number; body: Record<string, unknown> }> {
  const headers: Record<string, string> = {
    "content-type": "application/octet-stream",
    "content-length": String(options.contentLength ?? bytes.length),
  };
  if (options.token !== undefined) headers["x-auth-token"] = options.token;
  if (options.sha !== null) headers["x-frame-sha256"] = options.sha ?? digest(bytes);
  if (options.key) headers["idempotency-key"] = options.key;

  const response = await fetch(`${origin}/api/v1/dashboard/frame`, {
    method: "PUT",
    headers,
    body: asBody(bytes),
  });
  return { status: response.status, body: (await response.json()) as Record<string, unknown> };
}

async function status(): Promise<StatusBody> {
  return statusFrom(origin);
}

async function settle(): Promise<void> {
  await new Promise((resolve) => setTimeout(resolve, 40));
}

beforeEach(async () => {
  device = new MockDevice({ token: TOKEN, panelDelayMs: 10 });
  origin = await device.listen(0);
});

afterEach(async () => {
  await device.close();
});

describe("status route", () => {
  it("is unauthenticated and reports the contract shape", async () => {
    const body = await status();
    expect(body.api).toBe(2);
    expect(body.firmware).toBe("NOTE4C-mock 0.2");
    expect(body.provisioned).toBe(true);
    expect(body.lockdown).toBe(true);
    expect(body.stored).toMatchObject({ present: false, seq: 0 });
    expect(body.displayed).toMatchObject({ present: false, seq: 0 });
    expect(body.refresh).toMatchObject({ state: "idle", pending: false });
  });

  it("carries the capability list and truthful device metadata at api 2", async () => {
    const body = await status();
    expect(body.capabilities).toContain("config.v2");
    expect(body.capabilities).toContain("action.restart");
    // The inherited vendor strings are gone. This is the product, not the fork.
    expect(body.device?.name).toBe("NOTE4C mock panel");
    expect(body.device?.model).toBe("zectrix-s3-epaper-4.2");
    expect(body.device?.upstream_base).toBe("6.5.9");
    expect(JSON.stringify(body)).not.toContain("notellm");
    expect(JSON.stringify(body)).not.toContain("lazyyoun");
  });

  it("omits every api 2 field when the device speaks api 1", async () => {
    const old = new MockDevice({ token: TOKEN, api: 1, firmware: "6.5.9" });
    const oldOrigin = await old.listen(0);
    try {
      const body = await statusFrom(oldOrigin);
      expect(body.api).toBe(1);
      expect(body.capabilities).toBeUndefined();
      expect(body.device).toBeUndefined();
      expect(body.config_revision).toBeUndefined();
    } finally {
      await old.close();
    }
  });
});

describe("frame PUT authentication", () => {
  it("refuses a missing token", async () => {
    const result = await put(frame(0x55));
    expect(result.status).toBe(401);
    expect(result.body.error).toBe("unauthorized");
  });

  it("refuses a wrong token", async () => {
    const result = await put(frame(0x55), { token: "b".repeat(64) });
    expect(result.status).toBe(401);
  });

  it("refuses every mutating route when the device was never paired", async () => {
    const unpaired = new MockDevice({ token: null, panelDelayMs: 10 });
    const unpairedOrigin = await unpaired.listen(0);
    try {
      const response = await putTo(unpairedOrigin, frame(0x55));
      expect(response.status).toBe(503);
      expect((await response.json()).error).toBe("not_provisioned");
    } finally {
      await unpaired.close();
    }
  });

  it("locks out after ten failures in sixty seconds", async () => {
    for (let i = 0; i < AUTH_FAILURE_LIMIT; i += 1) {
      const result = await put(frame(0x55), { token: "b".repeat(64) });
      expect(result.status).toBe(401);
    }
    const lockedOut = await put(frame(0x55), { token: "b".repeat(64) });
    expect(lockedOut.status).toBe(429);
    expect(lockedOut.body.error).toBe("locked_out");
  });

  it("does not let a correct token bypass an active lockout", async () => {
    for (let i = 0; i < AUTH_FAILURE_LIMIT; i += 1) {
      await put(frame(0x55), { token: "b".repeat(64) });
    }
    const correct = await put(frame(0x55), { token: TOKEN });
    expect(correct.status).toBe(429);
  });
});

describe("frame PUT length", () => {
  it("accepts exactly 30000 bytes", async () => {
    const result = await put(frame(0x55), { token: TOKEN });
    expect(result.status).toBe(202);
    expect(result.body).toMatchObject({
      accepted: true,
      persisted: true,
      deduped: false,
      replay: false,
      seq: 1,
    });
  });

  it("refuses a short body", async () => {
    const short = new Uint8Array(29_999).fill(0x55);
    const result = await put(short, { token: TOKEN });
    expect(result.status).toBe(400);
    expect(result.body.error).toBe("bad_length");
  });

  it("refuses a body larger than 30000 bytes", async () => {
    const oversized = new Uint8Array(40_000).fill(0x55);
    const result = await put(oversized, { token: TOKEN });
    expect(result.status).toBe(413);
    expect(result.body.error).toBe("too_large");
  });
});

describe("sha verification", () => {
  it("refuses a body that does not match X-Frame-Sha256", async () => {
    const result = await put(frame(0x55), { token: TOKEN, sha: "f".repeat(64) });
    expect(result.status).toBe(422);
    expect(result.body.error).toBe("sha_mismatch");

    // Nothing was stored: the previous frame is still the active one.
    expect((await status()).stored.present).toBe(false);
  });

  it("accepts a PUT with no sha header at all", async () => {
    const result = await put(frame(0x55), { token: TOKEN, sha: null });
    expect(result.status).toBe(202);
  });
});

describe("dedup and idempotency", () => {
  it("dedups byte-identical frames without a repaint", async () => {
    const bytes = frame(0x55);
    const first = await put(bytes, { token: TOKEN });
    expect(first.status).toBe(202);
    await settle();

    const second = await put(bytes, { token: TOKEN });
    expect(second.status).toBe(200);
    expect(second.body).toMatchObject({
      deduped: true,
      replay: false,
      seq: 1,
      render: "skipped",
    });

    const after = await status();
    expect(after.stored.seq).toBe(1);
    expect(after.refresh.skipped).toBe(1);
    expect(after.refresh.renders).toBe(1);
  });

  it("replays a repeated idempotency key without repainting", async () => {
    const first = await put(frame(0x55), { token: TOKEN, key: "k1" });
    expect(first.status).toBe(202);
    await settle();

    // Different bytes, same key: the ring answers, the device does not repaint.
    const replayed = await put(frame(0xaa), { token: TOKEN, key: "k1" });
    expect(replayed.status).toBe(200);
    expect(replayed.body).toMatchObject({ replay: true, seq: 1 });
    expect((await status()).stored.seq).toBe(1);
  });

  it("remembers only the last eight keys", async () => {
    for (let i = 0; i < IDEMPOTENCY_RING_DEPTH + 2; i += 1) {
      await put(frame(i + 1), { token: TOKEN, key: `k${i}` });
      await settle();
    }
    // k0 has fallen out of the ring, so it is treated as a fresh request.
    const evicted = await put(frame(0x77), { token: TOKEN, key: "k0" });
    expect(evicted.status).toBe(202);
    expect(evicted.body.replay).toBe(false);
  });
});

describe("stored versus displayed", () => {
  it("reports stored immediately and displayed only after the panel cycle", async () => {
    const slow = new MockDevice({ token: TOKEN, panelDelayMs: 120 });
    const slowOrigin = await slow.listen(0);
    try {
      const bytes = frame(0x55);
      const response = await putTo(slowOrigin, bytes);
      expect(response.status).toBe(202);

      const during = await statusFrom(slowOrigin);
      expect(during.stored.sha256).toBe(digest(bytes));
      expect(during.displayed.sha256).toBe("");
      expect(during.refresh.state).toBe("rendering");

      await new Promise((resolve) => setTimeout(resolve, 200));
      const after = await statusFrom(slowOrigin);
      expect(after.displayed.sha256).toBe(digest(bytes));
      expect(after.refresh.state).toBe("idle");
      expect(after.timing_ms.panel).toBeGreaterThanOrEqual(100);
    } finally {
      await slow.close();
    }
  });

  it("keeps displayed on the previous frame when the ack fails", async () => {
    const broken = new MockDevice({
      token: TOKEN,
      panelDelayMs: 10,
      failAck: true,
    });
    const brokenOrigin = await broken.listen(0);
    try {
      const bytes = frame(0x55);
      await putTo(brokenOrigin, bytes);
      await new Promise((resolve) => setTimeout(resolve, 80));
      const after = await statusFrom(brokenOrigin);
      expect(after.stored.sha256).toBe(digest(bytes));
      // If BUSY never releases, the panel contents are genuinely unknown, so
      // displayed keeps reporting the frame still believed to be on the glass.
      expect(after.displayed.sha256).toBe("");
      expect(after.refresh.renders).toBe(0);
    } finally {
      await broken.close();
    }
  });

  it("coalesces a burst into two refreshes ending on the last frame", async () => {
    const slow = new MockDevice({ token: TOKEN, panelDelayMs: 60 });
    const slowOrigin = await slow.listen(0);
    try {
      for (let i = 1; i <= 6; i += 1) {
        await putTo(slowOrigin, frame(i));
      }
      await new Promise((resolve) => setTimeout(resolve, 300));
      const after = await statusFrom(slowOrigin);
      expect(after.stored.seq).toBe(6);
      expect(after.displayed.sha256).toBe(digest(frame(6)));
      expect(after.refresh.renders).toBe(2);
      expect(after.refresh.coalesced).toBeGreaterThan(0);
    } finally {
      await slow.close();
    }
  });
});

describe("frame read back", () => {
  it("returns the stored bytes with their digest", async () => {
    const bytes = frame(0x33);
    await put(bytes, { token: TOKEN });
    const response = await fetch(`${origin}/api/v1/dashboard/frame`);
    expect(response.status).toBe(200);
    expect(response.headers.get("x-frame-sha256")).toBe(digest(bytes));
    const read = new Uint8Array(await response.arrayBuffer());
    expect(read.length).toBe(FRAME_BYTES);
    expect(read).toEqual(bytes);
  });

  it("404s when nothing is stored", async () => {
    expect((await fetch(`${origin}/api/v1/dashboard/frame`)).status).toBe(404);
  });
});

describe("refresh route", () => {
  it("requires a token, because a repaint costs panel wear", async () => {
    await put(frame(0x55), { token: TOKEN });
    const unauthorised = await fetch(`${origin}/api/v1/dashboard/refresh`, {
      method: "POST",
    });
    expect(unauthorised.status).toBe(401);
  });

  it("404s when there is nothing to repaint", async () => {
    const response = await fetch(`${origin}/api/v1/dashboard/refresh`, {
      method: "POST",
      headers: { "x-auth-token": TOKEN },
    });
    expect(response.status).toBe(404);
  });

  it("repaints the stored frame", async () => {
    await put(frame(0x55), { token: TOKEN });
    await settle();
    const response = await fetch(`${origin}/api/v1/dashboard/refresh`, {
      method: "POST",
      headers: { "x-auth-token": TOKEN },
    });
    expect(response.status).toBe(202);
    await settle();
    expect((await status()).refresh.renders).toBe(2);
  });
});

describe("pairing", () => {
  it("refuses a claim when no physical window is open", async () => {
    const response = await fetch(`${origin}/api/v1/dashboard/pair`, {
      method: "POST",
    });
    expect(response.status).toBe(403);
    expect((await response.json()).error).toBe("not_pairing");
  });

  it("mints a token during an open window, then closes it", async () => {
    device.pairingWindowOpen = true;
    const first = await fetch(`${origin}/api/v1/dashboard/pair`, { method: "POST" });
    expect(first.status).toBe(200);
    expect((await first.json()).token).toMatch(/^[0-9a-f]{64}$/);

    const second = await fetch(`${origin}/api/v1/dashboard/pair`, { method: "POST" });
    expect(second.status).toBe(403);
  });
});

describe("voice hub", () => {
  it("requires a token and never echoes the value back", async () => {
    const unauthorised = await fetch(`${origin}/api/v1/voice/hub`, {
      method: "POST",
      body: JSON.stringify({ url: "http://x", token: "t" }),
    });
    expect(unauthorised.status).toBe(401);

    const response = await fetch(`${origin}/api/v1/voice/hub`, {
      method: "POST",
      headers: { "x-auth-token": TOKEN, "content-type": "application/json" },
      body: JSON.stringify({
        url: "http://127.0.0.1:8770",
        token: "hub-secret-value",
      }),
    });
    expect(response.status).toBe(200);
    const text = await response.text();
    expect(text).not.toContain("hub-secret-value");
    expect(JSON.parse(text)).toMatchObject({
      accepted: true,
      url: "http://127.0.0.1:8770",
      token_set: true,
    });
  });
});

describe("legacy gallery routes", () => {
  it("refuses legacy writes while lockdown is on, which is the default", async () => {
    const response = await fetch(`${origin}/upload`, { method: "POST", body: "x" });
    expect(response.status).toBe(403);
    expect((await response.json()).error).toBe("lockdown");
  });
});
