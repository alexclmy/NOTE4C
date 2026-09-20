import http from "node:http";
import { createHash } from "node:crypto";
import type { AddressInfo, Socket } from "node:net";
import { afterEach, describe, expect, it, vi } from "vitest";
import { MockDevice } from "@mock/server";
import { PACKED_BYTES } from "@/core/palette";
import { DeviceAddressError } from "@/server/device/address";
import {
  DeviceClient,
  DeviceError,
  DeviceUncertainError,
} from "@/server/device/client";
import { describeDeviceFailure } from "@/server/device/failure";
import {
  MAX_RESPONSE_BYTES,
  deviceRequest,
} from "@/server/device/transport";

/**
 * The device transport, and the outage it was written for.
 *
 * The failure this file exists to prevent from recurring: the physical panel
 * answered `/api/v1/dashboard/status` in 70-160 ms, a standalone script
 * reached it, and the same code inside a Next.js production server reported it
 * as unreachable — because the request went out through a *global* `fetch`
 * that the server runtime replaces, and because every possible cause collapsed
 * into one sentence with no way to tell a sleeping panel from a transport that
 * never left the machine.
 *
 * So there are two families of test here, and they are the two halves of the
 * fix:
 *
 *  1. The transport does not go through global `fetch`. Proven by breaking
 *     `globalThis.fetch` outright and pushing a real frame anyway.
 *  2. A failure says which failure it was, and the connect-versus-deadline
 *     classification that push semantics depend on is unchanged.
 */

const TOKEN = "e".repeat(64);
const STATUS_PATH = "/api/v1/dashboard/status";

interface Harness {
  origin: string;
  /** Every connection the server accepted. Length is the socket count. */
  connections: Socket[];
  requests: { method: string; url: string; headers: http.IncomingHttpHeaders }[];
  close: () => Promise<void>;
}

const openHarnesses: Harness[] = [];

/** A loopback server that records what arrived and answers however a test says. */
async function harness(
  handler: (
    request: http.IncomingMessage,
    response: http.ServerResponse,
  ) => void,
): Promise<Harness> {
  const connections: Socket[] = [];
  const requests: Harness["requests"] = [];
  const server = http.createServer((request, response) => {
    requests.push({
      method: request.method ?? "",
      url: request.url ?? "",
      headers: request.headers,
    });
    handler(request, response);
  });
  server.on("connection", (socket) => connections.push(socket));

  await new Promise<void>((resolve) => server.listen(0, "127.0.0.1", resolve));
  const port = (server.address() as AddressInfo).port;

  const instance: Harness = {
    origin: `http://127.0.0.1:${port}`,
    connections,
    requests,
    close: () =>
      new Promise<void>((resolve) => {
        server.closeAllConnections();
        server.close(() => resolve());
      }),
  };
  openHarnesses.push(instance);
  return instance;
}

/** A port with nothing behind it: bound, read, and released. */
async function closedPort(): Promise<number> {
  const server = http.createServer();
  await new Promise<void>((resolve) => server.listen(0, "127.0.0.1", resolve));
  const port = (server.address() as AddressInfo).port;
  await new Promise<void>((resolve) => server.close(() => resolve()));
  return port;
}

/** The digest the device checks the body against. */
function sha256(bytes: Uint8Array): string {
  return createHash("sha256").update(bytes).digest("hex");
}

function clientFor(origin: string, options: { timeoutMs?: number; token?: string } = {}) {
  return new DeviceClient({
    mode: "mock",
    address: "192.168.7.7",
    mockOrigin: origin,
    token: options.token ?? TOKEN,
    timeoutMs: options.timeoutMs,
  });
}

afterEach(async () => {
  vi.unstubAllGlobals();
  while (openHarnesses.length > 0) await openHarnesses.pop()?.close();
});

describe("independence from global fetch", () => {
  /**
   * The headline. Nothing in this test is subtle: `fetch` is replaced with a
   * function that throws, which is the worst case of an instrumented or
   * patched global, and every device call still works end to end.
   */
  it("reads status, config and a frame with global fetch replaced by a thrower", async () => {
    const device = new MockDevice({ token: TOKEN, panelDelayMs: 5 });
    const origin = await device.listen(0);
    const client = clientFor(origin);

    const broken = vi.fn(() => {
      throw new TypeError("fetch failed");
    });
    vi.stubGlobal("fetch", broken);

    try {
      const status = await client.status();
      expect(status.api).toBe(2);

      const config = await client.getConfig();
      expect(config.revision).toBe(0);

      const bytes = new Uint8Array(PACKED_BYTES).fill(0x55);
      const digest = sha256(bytes);
      const accepted = await client.putFrame(bytes, {
        sha256: digest,
        idempotencyKey: "transport-test-1",
      });
      expect(accepted.accepted).toBe(true);

      const read = await client.getFrame();
      expect(read.bytes.length).toBe(PACKED_BYTES);
      // The response headers survive as a Headers, which is what every caller
      // of getFrame() already reads.
      expect(read.sha256).toBe(digest);

      expect(broken).not.toHaveBeenCalled();
    } finally {
      await device.close();
    }
  });

  it("still sends X-Auth-Token, under that exact name, without a global fetch", async () => {
    const server = await harness((_request, response) => {
      response.writeHead(204).end();
    });
    vi.stubGlobal("fetch", () => {
      throw new Error("global fetch must not be used");
    });

    await expect(clientFor(server.origin).getConfig()).rejects.toThrow(DeviceError);
    expect(server.requests[0]?.headers["x-auth-token"]).toBe(TOKEN);
    // The bearer token in this system is the device-to-hub direction, and this
    // header is not it.
    expect(server.requests[0]?.headers.authorization).toBeUndefined();
  });
});

describe("one socket per call", () => {
  /**
   * The panel serves at most four sockets and runs its own web interface off
   * the same httpd, so a pooled connection this process holds open is one the
   * device cannot give to anybody else — and one the *device* closed while
   * idle is a socket the tower would later write a frame into and watch reset.
   * A one-shot script never meets that; a server that has been up for a week
   * meets it constantly.
   */
  it("opens a new connection per request and asks for it to be closed", async () => {
    const server = await harness((_request, response) => {
      response
        .writeHead(200, { "content-type": "application/json" })
        .end(JSON.stringify({ ok: true }));
    });

    const client = clientFor(server.origin);
    await client.refresh();
    await client.refresh();

    expect(server.requests).toHaveLength(2);
    expect(server.connections).toHaveLength(2);
    for (const request of server.requests) {
      expect(request.headers.connection).toBe("close");
    }
  });

  it("serialises calls process wide, so the device is never asked twice at once", async () => {
    let active = 0;
    let peak = 0;
    const server = await harness((_request, response) => {
      active += 1;
      peak = Math.max(peak, active);
      setTimeout(() => {
        active -= 1;
        response.writeHead(200).end(JSON.stringify({ accepted: true }));
      }, 25);
    });

    const client = clientFor(server.origin);
    await Promise.all([client.refresh(), client.refresh(), client.refresh()]);

    expect(server.requests).toHaveLength(3);
    expect(peak).toBe(1);
  });
});

describe("failure classification", () => {
  it("calls a refused connection unreachable, before send, and not notable", async () => {
    const port = await closedPort();
    const client = clientFor(`http://127.0.0.1:${port}`);

    const error = await client.status().catch((e: unknown) => e);
    expect(error).toBeInstanceOf(DeviceError);
    const device = error as DeviceError;
    expect(device.code).toBe("unreachable");
    // Nothing was sent, and here that is a fact rather than an assumption.
    expect(device.beforeSend).toBe(true);
    expect(device.transport?.fault).toBe("network");
    expect(device.transport?.connected).toBe(false);
    expect(device.transport?.requestSent).toBe(false);
    expect(device.transport?.errno).toBe("ECONNREFUSED");
    // The sentence names the cause instead of "The device could not be reached".
    expect(device.message).toContain("127.0.0.1");
    expect(device.message).toContain("refused");

    const failure = describeDeviceFailure(error);
    expect(failure.kind).toBe("absent");
    // A device that is away is this product's normal state, not an event.
    expect(failure.notable).toBe(false);
    expect(failure.deviceAnswered).toBe(false);
  });

  it("calls a deadline uncertain, not failed", async () => {
    const server = await harness(() => {
      // Accept, and never answer. This is the shape of a panel that took the
      // request and then went away mid-refresh.
    });
    const client = clientFor(server.origin, { timeoutMs: 120 });

    const error = await client.status().catch((e: unknown) => e);
    expect(error).toBeInstanceOf(DeviceUncertainError);
    expect((error as Error).message).toContain("120 ms");

    const failure = describeDeviceFailure(error);
    expect(failure.kind).toBe("uncertain");
    // "We do not know" is a state this product carries, not one it resolves.
    expect(failure.deviceAnswered).toBeNull();
    expect(failure.notable).toBe(true);
  });

  /**
   * The distinction the outage needed and `fetch` could not express: the far
   * end sent a status line and then stopped, which proves it was awake and
   * talking whatever else went wrong.
   */
  it("calls a device that began answering and then stopped a transport fault", async () => {
    const server = await harness((_request, response) => {
      response.writeHead(200, { "content-length": "400" });
      // Flushed on its own tick, then cut. Destroying in the same tick as the
      // write means nothing ever leaves the kernel buffer, and the client
      // would see a link that failed before any answer — which is the *other*
      // case, tested below.
      response.write("{", () => response.socket?.destroy());
    });
    const client = clientFor(server.origin);

    const error = await client.status().catch((e: unknown) => e);
    expect(error).toBeInstanceOf(DeviceError);
    const device = error as DeviceError;
    expect(device.code).toBe("unreachable");
    expect(device.transport?.connected).toBe(true);
    expect(device.transport?.answered).toBe(true);
    expect(device.transport?.requestSent).toBe(true);
    // The request went out in full, so claiming it never did would be a lie.
    expect(device.beforeSend).toBe(false);
    expect(device.message).toContain("not a sleeping panel");

    const failure = describeDeviceFailure(error);
    expect(failure.kind).toBe("transport");
    expect(failure.notable).toBe(true);
    expect(failure.deviceAnswered).toBe(true);
  });

  /**
   * The other side of that line, and the reason it is drawn at the status line
   * rather than at the TCP handshake. A socket accepted and then destroyed
   * before any answer is how a device dropping off Wi-Fi looks — and it is
   * exactly how the in-repo mock simulates deep sleep, so calling it a fault
   * would cry wolf in the mode the whole browser suite runs in.
   */
  it("still calls a reset before any answer an absent device, not a fault", async () => {
    const server = await harness((request) => {
      request.socket.destroy();
    });

    const error = await clientFor(server.origin).status().catch((e: unknown) => e);
    const device = error as DeviceError;
    expect(device.transport?.connected).toBe(true);
    expect(device.transport?.answered).toBe(false);

    const failure = describeDeviceFailure(error);
    expect(failure.kind).toBe("absent");
    expect(failure.notable).toBe(false);
    expect(failure.detail).not.toContain("not a sleeping panel");
  });

  /**
   * The same thing again, through the mock's own sleep switch rather than a
   * hand-rolled reset, because that is the path every other test and the whole
   * browser suite exercise.
   */
  it("reads the mock's simulated deep sleep as an absent device", async () => {
    const device = new MockDevice({ token: TOKEN });
    const origin = await device.listen(0);
    device.asleep = true;

    try {
      const error = await clientFor(origin).status().catch((e: unknown) => e);
      const failure = describeDeviceFailure(error);
      expect(failure.kind).toBe("absent");
      expect(failure.notable).toBe(false);
    } finally {
      await device.close();
    }
  });

  /**
   * The branch the outage actually lived in. Something inside this process
   * throws before the wire is involved; the old code reported that as a
   * sleeping device. Provoked here with a header value Node refuses to put on
   * a socket, which is the cheapest honest way to make the transport throw
   * something it did not expect.
   */
  it("says so when the tower's own transport is what failed", async () => {
    const server = await harness((_request, response) => {
      response.writeHead(200).end("{}");
    });
    const client = clientFor(server.origin, { token: "not\na\rtoken" });

    const error = await client.getConfig().catch((e: unknown) => e);
    expect(error).toBeInstanceOf(DeviceError);
    const device = error as DeviceError;
    expect(device.transport?.fault).toBe("tower");
    expect(device.transport?.errno).toBeNull();
    expect(device.message).toContain("own device transport failed");
    expect(server.requests).toHaveLength(0);

    const failure = describeDeviceFailure(error);
    // Not "absent". A tower-side fault must never read as a panel that is away.
    expect(failure.kind).toBe("transport");
    expect(failure.notable).toBe(true);
  });

  it("classifies an explicit device refusal as refused, not as a transport fault", async () => {
    const server = await harness((_request, response) => {
      response
        .writeHead(401, { "content-type": "application/json" })
        .end(JSON.stringify({ error: "unauthorised", detail: "Bad token" }));
    });

    const error = await clientFor(server.origin).getConfig().catch((e: unknown) => e);
    const failure = describeDeviceFailure(error);
    expect(failure.kind).toBe("refused");
    expect(failure.code).toBe("unauthorised");
    expect(failure.deviceAnswered).toBe(true);
  });

  it("classifies a device that answers off-contract as contract, not refused", async () => {
    const server = await harness((_request, response) => {
      response
        .writeHead(200, { "content-type": "application/json" })
        .end(JSON.stringify({ firmware: "1.0" }));
    });

    const error = await clientFor(server.origin).status().catch((e: unknown) => e);
    const failure = describeDeviceFailure(error);
    expect(failure.kind).toBe("contract");
    expect(failure.code).toBe("bad_status_shape");
    expect(failure.deviceAnswered).toBe(true);
  });

  it("keeps a missing token a configuration problem, not an unreachable device", async () => {
    const client = new DeviceClient({
      mode: "mock",
      address: "192.168.7.7",
      mockOrigin: "http://127.0.0.1:9",
      token: null,
    });

    const error = await client.getConfig().catch((e: unknown) => e);
    const failure = describeDeviceFailure(error);
    expect(failure.kind).toBe("not_configured");
    expect(failure.code).toBe("no_token");
  });
});

describe("what the transport refuses", () => {
  it("never follows a redirect, and refuses one loudly", async () => {
    const server = await harness((_request, response) => {
      response
        .writeHead(302, { location: "http://127.0.0.1:1/captive" })
        .end();
    });

    const error = await clientFor(server.origin).status().catch((e: unknown) => e);
    expect(error).toBeInstanceOf(DeviceError);
    expect((error as DeviceError).code).toBe("redirect_refused");
    expect((error as DeviceError).status).toBe(302);
    // One request. The redirect was read as a refusal, not as a hop.
    expect(server.requests).toHaveLength(1);
  });

  // Both guards throw before a socket, a timer or a promise exists, which is
  // why these are synchronous assertions.
  it("checks the path allowlist at the socket, not only in the caller", () => {
    expect(() =>
      deviceRequest({
        host: "127.0.0.1",
        port: 9,
        path: "/upload",
        method: "GET",
        headers: {},
        timeoutMs: 100,
      }),
    ).toThrow(DeviceAddressError);
  });

  it("refuses any address outside private and loopback space", () => {
    for (const host of ["8.8.8.8", "169.254.1.1", "example.invalid"]) {
      expect(() =>
        deviceRequest({
          host,
          port: 80,
          path: STATUS_PATH,
          method: "GET",
          headers: {},
          timeoutMs: 100,
        }),
      ).toThrow(DeviceAddressError);
    }
  });

  it("drops an answer that is larger than any frame could be", async () => {
    const server = await harness((_request, response) => {
      response.writeHead(200);
      response.end(Buffer.alloc(MAX_RESPONSE_BYTES + 1024));
    });
    const port = Number(new URL(server.origin).port);

    await expect(
      deviceRequest({
        host: "127.0.0.1",
        port,
        path: STATUS_PATH,
        method: "GET",
        headers: {},
        timeoutMs: 5_000,
      }),
    ).rejects.toThrow(/exceeded/);
  });
});

describe("bodies and headers on the wire", () => {
  it("sends a 30 000-byte frame verbatim and reads binary back", async () => {
    const device = new MockDevice({ token: TOKEN, panelDelayMs: 5 });
    const origin = await device.listen(0);
    const client = clientFor(origin);

    try {
      const bytes = new Uint8Array(PACKED_BYTES);
      for (let i = 0; i < bytes.length; i += 1) bytes[i] = i % 251;
      const sha = sha256(bytes);

      await client.putFrame(bytes, { sha256: sha, idempotencyKey: "binary-1" });
      const read = await client.getFrame();

      expect(read.bytes.length).toBe(PACKED_BYTES);
      expect(Buffer.from(read.bytes).equals(Buffer.from(bytes))).toBe(true);
      expect(read.sha256).toBe(sha);
    } finally {
      await device.close();
    }
  });

  it("sends a JSON body with the content length the device expects", async () => {
    const bodies: string[] = [];
    const server = await harness((request, response) => {
      const chunks: Buffer[] = [];
      request.on("data", (chunk: Buffer) => chunks.push(chunk));
      request.on("end", () => {
        bodies.push(Buffer.concat(chunks).toString("utf8"));
        response
          .writeHead(200, { "content-type": "application/json" })
          .end(JSON.stringify({ accepted: true, url: "http://hub", token_set: true }));
      });
    });

    await clientFor(server.origin).setVoiceHub("http://hub", "hub-token");

    expect(bodies[0]).toBe(JSON.stringify({ url: "http://hub", token: "hub-token" }));
    expect(server.requests[0]?.headers["content-length"]).toBe(
      String(Buffer.byteLength(bodies[0] ?? "")),
    );
    expect(server.requests[0]?.headers["content-type"]).toBe("application/json");
  });

  it("reports the device's status code as the device sent it", async () => {
    const server = await harness((_request, response) => {
      response
        .writeHead(409, { "content-type": "application/json" })
        .end(JSON.stringify({ error: "revision_mismatch", revision: 7 }));
    });

    await expect(clientFor(server.origin).patchConfig(3, { "voice.muted": true }))
      .rejects.toMatchObject({ code: "revision_mismatch", deviceRevision: 7 });
  });
});
