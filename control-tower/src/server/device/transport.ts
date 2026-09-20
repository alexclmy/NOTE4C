import http from "node:http";
import {
  DeviceAddressError,
  assertAllowedPath,
  isIpv4Literal,
  isLoopback,
  isRfc1918,
} from "./address";

/**
 * The one socket this product opens at the panel, written on `node:http`.
 *
 * This used to be `fetch`, and the reason it no longer is cost a production
 * outage to learn. `fetch` is a *global*, and the tower's device calls run
 * inside a Next.js server that replaces that global with an instrumented one
 * (`next/dist/server/lib/patch-fetch.js`, installed under
 * `Symbol.for("next-patch")`) and shares a single process-wide undici
 * dispatcher — connection pool, keep-alive and all — with every other fetch in
 * the process. The same `DeviceClient.status()` that answered in 70 ms under
 * `npx tsx` was caught as "unreachable" inside `next start`, because the
 * failure was never the device: it was everything the tower's request passed
 * through on the way out, none of which the tower controls, and none of which
 * is present in a one-shot script.
 *
 * So the device transport now depends on nothing a framework, an APM agent or
 * an import order can substitute. Three properties follow from that, and they
 * are the point of the file:
 *
 *  - **No global.** `http.request` is taken off the `node:http` module object
 *    at call time. Patching `globalThis.fetch` cannot reach this code — which
 *    is what the regression test in tests/unit/deviceTransport.test.ts pins.
 *  - **No shared pool.** `agent: false` gives every call its own socket and
 *    sends `Connection: close`. The panel serves at most four sockets and runs
 *    its own web interface off the same httpd, so a pooled connection held
 *    open by this process is one the device cannot give to anyone else — and a
 *    pooled connection the *device* closed while idle is a socket this process
 *    would later write a frame into and watch reset. A long-lived server hits
 *    that; a script that exits after one request never can.
 *  - **No DNS, ever.** The host is asserted to be an IPv4 literal in private
 *    or loopback space *here*, at the line that opens the socket, not only in
 *    the caller. `dns.lookup` short-circuits an IP literal without a query, so
 *    with that assertion in place there is no name resolution between the
 *    address check and the connection, and the DNS-rebinding class stays gone.
 *
 * What this file does NOT do is policy. It does not know about tokens, allowed
 * status codes, retries or redirect *refusal* — only that it never follows one.
 * That all stays in DeviceClient, where it was.
 */

/** Where in one exchange something went wrong. */
export type TransportPhase =
  /** No connection was ever established. Nothing was sent. */
  | "connect"
  /** Connected, and the request was still going out. */
  | "request"
  /** The request was written; the answer was being read. */
  | "response";

/**
 * A generous ceiling on what the tower will read back.
 *
 * The largest legitimate response is a 30 000-byte frame. This is not a guard
 * against the panel, which cannot produce anything near it; it is a guard
 * against whatever else might be answering on that address, so a raw transport
 * cannot be talked into buffering an unbounded stream.
 */
export const MAX_RESPONSE_BYTES = 2 * 1024 * 1024;

/**
 * The five statuses the fetch spec calls redirects.
 *
 * `http.request` structurally cannot follow one, which is the guarantee; this
 * list is what lets DeviceClient *refuse* one loudly instead of parsing a
 * captive portal's HTML as a device response.
 */
export const REDIRECT_STATUSES: readonly number[] = [301, 302, 303, 307, 308];

/**
 * The errnos an operating system raises out of its own routing tables, before
 * a packet exists.
 *
 * None of them is a fact about the panel. They are this machine saying it has
 * nowhere to send the bytes: no neighbour entry, no route, or a neighbour it
 * has already decided is down.
 */
export const LOCAL_ROUTING_ERRNOS: readonly string[] = [
  "EHOSTUNREACH",
  "EHOSTDOWN",
  "ENETUNREACH",
  "ENETDOWN",
];

/**
 * Below this, a routing errno is a *cached* verdict rather than a fresh one.
 *
 * BSD — and therefore macOS — remembers that a neighbour did not answer ARP and
 * refuses to try again for `net.link.ether.inet.host_down_time` seconds, which
 * is 20 on the production machine. Inside that window every `connect(2)` to the
 * address returns instantly, out of the kernel's own table, with no ARP request
 * and no packet on the wire. Measured on the production host, against an
 * address nothing on that LAN answers for: a first attempt blocks for seconds
 * while ARP is actually tried, and attempts at +0 s, +2 s and +7 s afterwards
 * fail in 4 ms, 2 ms and 1 ms.
 *
 * A genuine determination cannot be that fast: resolving a neighbour that is
 * not there costs `net.link.ether.inet.maxtries` probes a second apart. So a
 * routing failure under this threshold means the kernel answered from memory,
 * and the tower learned *nothing* about the panel — which is exactly the
 * failure the production captures show, at 0 ms and 1 ms, while the panel was
 * answering other clients on the same LAN in 55 ms.
 *
 * Fifty milliseconds rather than five: the margin costs nothing, because the
 * alternative it has to stay clear of is measured in seconds.
 */
export const LOCAL_VERDICT_MAX_MS = 50;

/** The transport failed. Carries what a diagnosis needs, and nothing else. */
export class DeviceTransportError extends Error {
  readonly phase: TransportPhase;
  /** The OS error code, e.g. ECONNREFUSED. null when there was not one. */
  readonly errno: string | null;
  readonly syscall: string | null;
  /** True when the request bytes were all written to the socket. */
  readonly requestSent: boolean;
  readonly elapsedMs: number;
  /**
   * True when this machine refused the call out of its own routing state,
   * without asking the network. See LOCAL_VERDICT_MAX_MS.
   *
   * It is deliberately a property of the error rather than something each
   * caller re-derives: it changes what may be *said* about the device (nothing)
   * and what a read-only retry is worth (a great deal — the verdict expires).
   */
  readonly heldLocally: boolean;

  constructor(
    message: string,
    detail: {
      phase: TransportPhase;
      errno?: string | null;
      syscall?: string | null;
      requestSent: boolean;
      elapsedMs: number;
    },
  ) {
    super(message);
    this.name = "DeviceTransportError";
    this.phase = detail.phase;
    this.errno = detail.errno ?? null;
    this.syscall = detail.syscall ?? null;
    this.requestSent = detail.requestSent;
    this.elapsedMs = detail.elapsedMs;
    this.heldLocally =
      this.phase === "connect" &&
      this.errno !== null &&
      LOCAL_ROUTING_ERRNOS.includes(this.errno) &&
      this.elapsedMs <= LOCAL_VERDICT_MAX_MS;
  }
}

/**
 * The deadline ran out. Its own type because the answer is "we do not know",
 * not "it failed": DeviceClient turns this into DeviceUncertainError, which is
 * the state a push whose outcome nobody observed has to carry.
 */
export class DeviceTransportTimeout extends Error {
  readonly phase: TransportPhase;
  readonly requestSent: boolean;
  readonly timeoutMs: number;

  constructor(detail: {
    phase: TransportPhase;
    requestSent: boolean;
    timeoutMs: number;
  }) {
    super(`The device did not answer within ${detail.timeoutMs} ms`);
    this.name = "DeviceTransportTimeout";
    this.phase = detail.phase;
    this.requestSent = detail.requestSent;
    this.timeoutMs = detail.timeoutMs;
  }
}

export interface TransportRequest {
  /** An IPv4 literal. Asserted, not trusted. */
  host: string;
  port: number;
  /** Must be in ALLOWED_PATHS. Asserted here too, at the socket. */
  path: string;
  method: "GET" | "PUT" | "POST" | "PATCH";
  headers: Record<string, string>;
  body?: Uint8Array | string;
  /** One deadline for connect, send and read together. */
  timeoutMs: number;
}

export interface TransportResponse {
  status: number;
  headers: Headers;
  /** The whole body. Empty, never null, when there was none. */
  body: Uint8Array;
  elapsedMs: number;
}

/**
 * The address rule, enforced where the connection is made.
 *
 * `resolveEndpoint` already decided this and it is still the only thing that
 * builds an endpoint. Re-asserting costs two comparisons and means the SSRF
 * guarantee is a property of the code that opens sockets rather than of a
 * call path somebody might add later.
 */
function assertDeviceHost(host: string): void {
  if (!isIpv4Literal(host)) {
    throw new DeviceAddressError(
      "The device transport will only open a socket to a plain IPv4 address",
    );
  }
  if (!isRfc1918(host) && !isLoopback(host)) {
    throw new DeviceAddressError(
      `The device transport refuses ${host}: only private RFC1918 space and loopback are reachable`,
    );
  }
}

function bodyBuffer(body: Uint8Array | string): Buffer {
  if (typeof body === "string") return Buffer.from(body, "utf8");
  // A view onto the caller's bytes, not a copy: a frame is 30 000 bytes and
  // there is no reason to duplicate it on the way to the socket.
  return Buffer.from(body.buffer, body.byteOffset, body.byteLength);
}

/**
 * Node's header bag, as a Headers.
 *
 * `Headers` is what every caller already reads (`x-frame-sha256` on a frame
 * GET), so the transport keeps that shape rather than making every call site
 * learn a second one. A name or value the WHATWG parser rejects is a fault of
 * whatever answered, not of the tower, so it is reported as a response-phase
 * transport failure instead of thrown as a bare TypeError.
 */
function toHeaders(raw: http.IncomingHttpHeaders): Headers {
  const headers = new Headers();
  for (const [name, value] of Object.entries(raw)) {
    if (value === undefined) continue;
    if (Array.isArray(value)) {
      for (const item of value) headers.append(name, item);
    } else {
      headers.append(name, value);
    }
  }
  return headers;
}

/**
 * One request, one socket, one deadline.
 *
 * Note that the two guards below throw *synchronously*, before a promise, a
 * socket or a timer exists — fail-fast, like `assertAllowedPath` itself. A
 * caller that only handles rejections would miss them; DeviceClient calls this
 * inside a `try`, which catches both.
 */
export function deviceRequest(
  request: TransportRequest,
): Promise<TransportResponse> {
  // Both guards run before anything is allocated, and both throw
  // DeviceAddressError, which DeviceClient deliberately does not convert into
  // "unreachable": a refused path or a refused address is a tower bug or a
  // misconfiguration, never a sleeping device.
  assertAllowedPath(request.path);
  assertDeviceHost(request.host);

  const started = Date.now();

  return new Promise<TransportResponse>((resolve, reject) => {
    let settled = false;
    let connected = false;
    let requestSent = false;
    let responseStarted = false;

    const phase = (): TransportPhase =>
      !connected ? "connect" : !responseStarted ? "request" : "response";

    const outbound = http.request({
      host: request.host,
      port: request.port,
      path: request.path,
      method: request.method,
      headers: request.headers,
      // A fresh socket per call, and `Connection: close` with it. See the note
      // at the top of this file: the panel's four sockets are not ours to pool.
      agent: false,
      // Node sends `Host: <host>` (no port, for port 80) from these options,
      // which is what fetch sent too.
      setHost: true,
    });

    const timer = setTimeout(() => {
      fail(
        new DeviceTransportTimeout({
          phase: phase(),
          requestSent,
          timeoutMs: request.timeoutMs,
        }),
      );
    }, request.timeoutMs);
    // Nothing here should hold a process open past its deadline.
    timer.unref?.();

    function fail(error: Error): void {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      // Destroying emits another `error` on the request; `settled` is what
      // stops that second one from being reported over the first, which is the
      // one that actually says what happened.
      outbound.destroy();
      reject(error);
    }

    function succeed(response: TransportResponse): void {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      resolve(response);
    }

    outbound.on("socket", (socket) => {
      // Small writes to a device that answers in tens of milliseconds: waiting
      // 40 ms for a peer ACK to coalesce them buys nothing.
      socket.setNoDelay(true);

      // Receiving a Socket object does not prove TCP connected. In production
      // macOS can assign the socket with `connecting === false` immediately
      // before reporting EHOSTUNREACH. The former check marked that as a
      // request-phase failure, which suppressed the one safe status retry.
      // A remote address or the actual `connect` event is the evidence.
      if (socket.remoteAddress) connected = true;
      else socket.once("connect", () => { connected = true; });
    });

    // `finish` is the honest answer to "did the request go out". It is the
    // difference between a failure that definitely sent nothing and one that
    // wrote a whole frame and then lost the connection, and `fetch` could not
    // tell the two apart at all.
    outbound.on("finish", () => { requestSent = true; });

    outbound.on("error", (error: NodeJS.ErrnoException) => {
      fail(
        new DeviceTransportError(error.message, {
          phase: phase(),
          errno: error.code ?? null,
          syscall: error.syscall ?? null,
          requestSent,
          elapsedMs: Date.now() - started,
        }),
      );
    });

    outbound.on("response", (response) => {
      responseStarted = true;

      let headers: Headers;
      try {
        headers = toHeaders(response.headers);
      } catch (error) {
        fail(
          new DeviceTransportError(
            `The device sent a header the tower could not read: ${
              error instanceof Error ? error.message : "unknown"
            }`,
            {
              phase: "response",
              requestSent,
              elapsedMs: Date.now() - started,
            },
          ),
        );
        return;
      }

      const chunks: Buffer[] = [];
      let size = 0;

      response.on("data", (chunk: Buffer) => {
        size += chunk.length;
        if (size > MAX_RESPONSE_BYTES) {
          fail(
            new DeviceTransportError(
              `The answer exceeded ${MAX_RESPONSE_BYTES} bytes and was dropped unread`,
              {
                phase: "response",
                requestSent,
                elapsedMs: Date.now() - started,
              },
            ),
          );
          return;
        }
        chunks.push(chunk);
      });

      response.on("aborted", () => {
        fail(
          new DeviceTransportError(
            "The device closed the connection before the answer was complete",
            {
              phase: "response",
              errno: "ECONNRESET",
              requestSent,
              elapsedMs: Date.now() - started,
            },
          ),
        );
      });

      response.on("error", (error: NodeJS.ErrnoException) => {
        fail(
          new DeviceTransportError(error.message, {
            phase: "response",
            errno: error.code ?? null,
            syscall: error.syscall ?? null,
            requestSent,
            elapsedMs: Date.now() - started,
          }),
        );
      });

      response.on("end", () => {
        succeed({
          status: response.statusCode ?? 0,
          headers,
          body: chunks.length === 1 ? chunks[0]! : Buffer.concat(chunks),
          elapsedMs: Date.now() - started,
        });
      });
    });

    if (request.body === undefined) outbound.end();
    else outbound.end(bodyBuffer(request.body));
  });
}
