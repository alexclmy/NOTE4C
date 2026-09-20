import { z } from "zod";
import { PACKED_BYTES } from "@/core/palette";
import { DevicePowerSchema } from "@/core/power";
import {
  DeviceAddressError,
  assertAllowedPath,
  resolveEndpoint,
  type DeviceMode,
  type ResolvedEndpoint,
} from "./address";
import {
  DeviceTransportError,
  DeviceTransportTimeout,
  REDIRECT_STATUSES,
  deviceRequest,
  type TransportPhase,
} from "./transport";

export const DEVICE_TIMEOUT_MS = 10_000;
/**
 * A complete 30 kB frame upload to the ESP32 can legitimately take longer
 * than a small status/config request. Keep reads bounded at ten seconds, but
 * do not abort a frame while the device is still consuming its body.
 */
export const DEVICE_FRAME_UPLOAD_TIMEOUT_MS = 60_000;

/**
 * How long a *status read* may keep retrying a purely local routing refusal.
 *
 * Two budgets, and the difference between them is who is waiting.
 *
 * `STATUS_RETRY_BUDGET_MS` is the background one, and it is deliberately tiny:
 * a wake can race ARP and Wi-Fi association by a few tens of milliseconds, so
 * one quick second attempt is worth having, and anything longer would have the
 * scheduler sitting on the device mutex for no good reason. This is the
 * behaviour that was already here.
 *
 * `MANUAL_STATUS_RETRY_BUDGET_MS` is for a person who pressed "Check now", and
 * it is sized against the thing it has to outlast: after one failed ARP
 * resolution the production host refuses every connection to that address for
 * `net.link.ether.inet.host_down_time` = 20 seconds, out of its own table, in
 * about a millisecond (see LOCAL_VERDICT_MAX_MS). Pressing BOOT and asking
 * immediately lands squarely inside that window — the panel's radio has not
 * associated yet when the click happens, the failure arms the hold-down, and
 * every read for the next twenty seconds fails instantly while the panel comes
 * up and answers everything else on the LAN. Twenty-one seconds of read-only
 * retries is what it takes to get past it and give the person the true answer
 * instead of a fast wrong one.
 *
 * The budget is only ever spent on failures diagnosed as local verdicts. A
 * device that genuinely does not answer fails slowly — ARP is actually tried —
 * so a sleeping panel still returns after one attempt, as it always did.
 */
export const STATUS_RETRY_BUDGET_MS = 150;
export const MANUAL_STATUS_RETRY_BUDGET_MS = 21_000;

/**
 * How long to wait before each further status attempt, in order.
 *
 * Exported so a test can assert the one property that matters about the
 * numbers: they add up to more than the hold-down they exist to outlast.
 */
export const STATUS_RETRY_BACKOFF_MS: readonly number[] = [
  150, 850, 2_000, 3_000, 5_000, 5_000, 5_000,
];

/**
 * Device metadata, api 2 and later.
 *
 * Optional on the schema because an api 1 device does not send it, and the
 * tower has to keep working against one rather than refusing to parse it.
 * Where this is absent the About page says the device did not report a value;
 * it does not fall back to a vendor name the tower made up.
 */
export const DeviceMetaSchema = z.object({
  name: z.string(),
  model: z.string(),
  hardware: z.string().optional(),
  panel: z.string().optional(),
  fw: z.string().optional(),
  upstream_base: z.string().optional(),
});
export type DeviceMeta = z.infer<typeof DeviceMetaSchema>;

export const DeviceStatusSchema = z.object({
  firmware: z.string(),
  api: z.number().int(),
  /** api 2. The tower gates every v2 control on membership of this list. */
  capabilities: z.array(z.string()).optional(),
  device: DeviceMetaSchema.optional(),
  config_revision: z.number().int().nonnegative().optional(),
  /**
   * The hybrid low-power block, present when the build advertises
   * `power.hybrid.v1`. Optional because a device without the contract omits
   * it, and nullable because the firmware emits an explicit null rather than a
   * truncated object if it could not render one: a tower that sees null knows
   * it learned nothing, where a fragment would fail to parse and take the
   * whole status response down with it.
   */
  power: DevicePowerSchema.nullish().transform((v) => v ?? null),
  initialised: z.boolean().optional(),
  provisioned: z.boolean(),
  lockdown: z.boolean(),
  stored: z.object({
    present: z.boolean(),
    seq: z.number().int(),
    sha256: z.string(),
    source_epoch: z.number().int().optional(),
  }),
  displayed: z.object({
    present: z.boolean(),
    seq: z.number().int(),
    sha256: z.string(),
  }),
  refresh: z.object({
    state: z.string(),
    pending: z.boolean(),
    renders: z.number().int(),
    skipped: z.number().int(),
    coalesced: z.number().int(),
  }),
  timing_ms: z.record(z.string(), z.number()).optional(),
  storage: z.record(z.string(), z.number()).optional(),
});
export type DeviceStatus = z.infer<typeof DeviceStatusSchema>;

export const FramePutResponseSchema = z.object({
  accepted: z.boolean(),
  persisted: z.boolean().optional(),
  deduped: z.boolean().optional(),
  replay: z.boolean().optional(),
  seq: z.number().int().optional(),
  sha256: z.string().optional(),
  render: z.string().optional(),
});
export type FramePutResponse = z.infer<typeof FramePutResponseSchema>;

/**
 * The readable configuration, api 2.
 *
 * `hub_token_set` is a boolean and there is no field anywhere in this schema
 * that could hold a token. A response that somehow carried one would be
 * dropped here rather than reaching a React prop.
 */
export const DeviceConfigSchema = z.object({
  api: z.number().int(),
  revision: z.number().int().nonnegative(),
  config: z.object({
    gallery: z.object({ slide_min: z.number().int() }),
    sync: z.object({ sync_interval: z.number().int() }),
    voice: z.object({
      muted: z.boolean(),
      hub_url: z.string(),
      hub_token_set: z.boolean(),
    }),
    dashboard: z.object({ lockdown: z.boolean() }),
    network: z.object({
      lan_service: z.boolean(),
      wifi_writable: z.boolean().optional(),
    }),
    /**
     * The *base* mode only. An open interactive window is live state with a
     * deadline and is reported on the status route next to the countdown that
     * makes it meaningful, never here among the settings that survive a reboot.
     */
    power: z
      .object({
        mode: z.string(),
        interactive_min: z.number().int(),
        wake_interval_min: z.number().int(),
      })
      .optional(),
  }),
});
export type DeviceConfig = z.infer<typeof DeviceConfigSchema>;

/** How a written field actually takes effect, in the device's own words. */
export const APPLY_MODES = [
  "immediate",
  "immediate_not_persisted",
  "restart_required",
] as const;
export type ApplyMode = (typeof APPLY_MODES)[number];

export const ConfigPatchResponseSchema = DeviceConfigSchema.extend({
  applied: z.record(z.string(), z.enum(APPLY_MODES)).optional(),
});
export type ConfigPatchResponse = z.infer<typeof ConfigPatchResponseSchema>;

/** Values the tower may send. Mirrors the firmware allowlist exactly. */
export type ConfigPatchValues = Record<string, number | boolean | string>;

/**
 * The device refused a write because somebody else changed the configuration
 * first. Its own type, because the product response is "go and look", not
 * "try again harder".
 */
export class DeviceRevisionConflict extends Error {
  readonly code = "revision_mismatch";
  readonly deviceRevision: number | null;
  constructor(deviceRevision: number | null) {
    super(
      "The device configuration changed since it was read. Nothing was written.",
    );
    this.name = "DeviceRevisionConflict";
    this.deviceRevision = deviceRevision;
  }
}

/**
 * What the transport saw, when the transport is what failed.
 *
 * This exists because "unreachable" was carrying two completely different
 * facts and telling nobody which one it held: a device asleep with its radio
 * off, and the tower's own HTTP layer failing on the way out. The first is
 * this product's normal state and not an event; the second is a fault, and it
 * is the one that was invisible for the whole of a production outage.
 *
 * Nothing here can hold a secret. The endpoint is the address the interface
 * already displays, and no header — least of all X-Auth-Token — is recorded.
 */
export interface DeviceTransportDiagnostics {
  /**
   * Which side of the socket the fault was on.
   *
   * `network` means the transport ran and the wire said no. `tower` means this
   * process failed before the wire was involved — an unexpected throw from
   * inside the transport, an instrumented module, a bug in the client. The
   * distinction is explicit rather than inferred from a missing errno, because
   * it is the exact question the outage could not answer, and a reader should
   * not have to guess it from the absence of a field.
   */
  fault: "network" | "tower";
  phase: TransportPhase;
  /** The OS error code, e.g. ECONNREFUSED. null when there was not one. */
  errno: string | null;
  syscall: string | null;
  /** True when the request bytes were all written to the socket. */
  requestSent: boolean;
  /** True when a TCP connection to the device was established. */
  connected: boolean;
  /**
   * True when the far end sent a status line, which is the only thing that
   * *proves* a device was awake and talking.
   *
   * Not the same as `connected`, and the gap between them is deliberate. A
   * link that drops between the SYN/ACK and the first byte of the answer looks
   * identical to a device that was never there — it is also exactly how the
   * in-repo mock simulates deep sleep, by destroying the socket it just
   * accepted — so a reset with nothing answered is classified as an absent
   * device rather than as a fault. See `describeDeviceFailure`.
   */
  answered: boolean;
  /**
   * True when this machine refused the call out of its own routing state and
   * no packet was ever sent.
   *
   * This is not a weaker form of "the device did not answer": it is the
   * absence of a question. An operating system that has decided a neighbour is
   * down answers every connection to it instantly, from memory, for a
   * hold-down period — so a reading with this flag set says nothing whatsoever
   * about the panel, and in particular is not evidence that it is asleep.
   */
  heldLocally: boolean;
  /** host:port. */
  endpoint: string;
  elapsedMs: number;
}

export class DeviceError extends Error {
  readonly code: string;
  readonly status: number | null;
  /** True when the request definitely never reached the device. */
  readonly beforeSend: boolean;
  /**
   * Present only on a transport failure. Optional rather than required so the
   * dozens of contract errors in this file are unchanged, and so a route that
   * only wants `error.message` still gets a sentence naming the actual cause.
   */
  readonly transport: DeviceTransportDiagnostics | null;

  constructor(
    code: string,
    message: string,
    status: number | null = null,
    beforeSend = false,
    transport: DeviceTransportDiagnostics | null = null,
  ) {
    super(message);
    this.name = "DeviceError";
    this.code = code;
    this.status = status;
    this.beforeSend = beforeSend;
    this.transport = transport;
  }
}

/**
 * The outcome of a write whose result we could not observe. This is its own
 * type, not an error subclass, because "we do not know" is a state the product
 * has to carry, not an exception to swallow.
 */
export class DeviceUncertainError extends Error {
  readonly code = "uncertain";
  constructor(message: string) {
    super(message);
    this.name = "DeviceUncertainError";
  }
}

/**
 * One sentence naming what actually happened on the wire.
 *
 * Every one of these used to be the string "The device could not be reached",
 * which is why an outage where the device was answering in 70 ms read, for
 * days, as a device that was asleep. The routes that surface a failure all
 * show `error.message` (see app/api/device/config, mode, actions and
 * diagnostics), so naming the cause here is what puts it in front of a person
 * without touching four route handlers.
 *
 * Each sentence ends by saying whether anything was sent, because that is the
 * question a reader looking at a queued frame actually has.
 */
function describeTransportFailure(
  error: DeviceTransportError,
  endpoint: string,
): string {
  const errno = error.errno;

  // The one that had to be said out loud. Everything below describes something
  // that happened on a network; this describes something that did not. The
  // previous sentence for it — "there is no route from this machine to
  // <panel-ip>" — was true and read, to everybody who saw it, as a statement
  // about the panel. It is a statement about this machine's memory of a failed
  // ARP resolution, which it holds for about twenty seconds and which the
  // tower's own thirty-second polling keeps re-arming.
  if (error.heldLocally) {
    return `This machine refused the connection to ${endpoint} from its own routing table, in ${error.elapsedMs} ms${
      errno ? ` (${errno})` : ""
    }: it has no current address-resolution entry for it and will not try again for a few seconds. Nothing was sent, and nothing was learned about the panel — it may be awake and answering. Reading again once the hold-down expires is the only way to find out.`;
  }

  if (error.phase === "connect") {
    switch (errno) {
      case "ECONNREFUSED":
        return `Nothing is listening at ${endpoint}: the connection was refused after ${error.elapsedMs} ms. Nothing was sent.`;
      case "EHOSTUNREACH":
        return `There is no route from this machine to ${endpoint} (EHOSTUNREACH). Nothing was sent.`;
      case "ENETUNREACH":
        return `This machine has no route to the network ${endpoint} is on (ENETUNREACH). Nothing was sent.`;
      case "ETIMEDOUT":
        return `${endpoint} did not accept a connection within ${error.elapsedMs} ms (ETIMEDOUT). Nothing was sent.`;
      case "EACCES":
        return `This machine refused to open a socket to ${endpoint} (EACCES). Nothing was sent.`;
      default:
        return `The tower could not open a connection to ${endpoint} after ${error.elapsedMs} ms${
          errno ? ` (${errno})` : ""
        }. Nothing was sent.`;
    }
  }

  if (error.phase === "request") {
    // Deliberately not "the device was there". A link that drops between the
    // handshake and the first byte back is indistinguishable from a device
    // that went to sleep a moment earlier, and claiming otherwise would be
    // the confident wrong answer this product is built not to give.
    return `${endpoint} accepted a connection and then the link failed before any answer came back, after ${error.elapsedMs} ms${
      errno ? ` (${errno})` : ""
    }. ${
      error.requestSent
        ? "The whole request went out, so the device may have acted on it."
        : "The request was still going out."
    }`;
  }

  return `${endpoint} began answering and then stopped, after ${error.elapsedMs} ms${
    errno ? ` (${errno})` : ""
  }. The device was there and talking, so this is not a sleeping panel.`;
}

export interface DeviceClientOptions {
  mode: DeviceMode;
  address: string;
  mockOrigin?: string | null;
  token?: string | null;
  timeoutMs?: number;
  frameUploadTimeoutMs?: number;
}

/**
 * v1 client.
 *
 * The device serves at most four sockets and its own UI is a client of that
 * same server, so every call goes through one single-flight mutex. Starving
 * the panel's own HTTP client to save a few milliseconds here would be a poor
 * trade.
 */
export class DeviceClient {
  private readonly endpoint: ResolvedEndpoint;
  private readonly token: string | null;
  private readonly timeoutMs: number;
  private readonly frameUploadTimeoutMs: number;
  readonly mode: DeviceMode;

  private static inFlight: Promise<unknown> = Promise.resolve();

  constructor(options: DeviceClientOptions) {
    this.mode = options.mode;
    this.endpoint = resolveEndpoint(
      options.mode,
      options.address,
      options.mockOrigin,
    );
    this.token = options.token ?? null;
    this.timeoutMs = options.timeoutMs ?? DEVICE_TIMEOUT_MS;
    this.frameUploadTimeoutMs =
      options.frameUploadTimeoutMs ??
      options.timeoutMs ??
      DEVICE_FRAME_UPLOAD_TIMEOUT_MS;
  }

  get origin(): string {
    return this.endpoint.origin;
  }

  /**
   * What a failure sentence calls the far end.
   *
   * The device's own address, which the interface already displays, and never
   * a header or a token — see the note on DeviceTransportDiagnostics.
   */
  private get endpointLabel(): string {
    return `${this.endpoint.host}:${this.endpoint.port}`;
  }

  /** Serialise every device call, process wide. */
  private static single<T>(work: () => Promise<T>): Promise<T> {
    const next = DeviceClient.inFlight.then(work, work);
    // Keep the chain alive even when a call rejects.
    DeviceClient.inFlight = next.then(
      () => undefined,
      () => undefined,
    );
    return next;
  }

  private async request(
    path: string,
    init: {
      method: "GET" | "PUT" | "POST" | "PATCH";
      body?: Uint8Array | string;
      headers?: Record<string, string>;
      auth?: boolean;
      expectBinary?: boolean;
      timeoutMs?: number;
    },
  ): Promise<{ status: number; json: unknown; bytes: Uint8Array | null; headers: Headers }> {
    assertAllowedPath(path);

    const headers: Record<string, string> = { ...init.headers };
    if (init.auth) {
      if (!this.token) {
        throw new DeviceError(
          "no_token",
          "No device token is configured in the tower",
          null,
          true,
        );
      }
      // The real header is X-Auth-Token, not Authorization: Bearer. The bearer
      // token in this system is the device-to-hub direction.
      headers["x-auth-token"] = this.token;
    }

    try {
      const response = await deviceRequest({
        host: this.endpoint.host,
        port: this.endpoint.port,
        path,
        method: init.method,
        headers,
        body: init.body,
        timeoutMs: init.timeoutMs ?? this.timeoutMs,
      });

      // Redirects are never followed — `http.request` cannot — and they are
      // refused rather than parsed. The firmware has no redirect on any route,
      // so a 302 here is something else answering on that address, and reading
      // a captive portal's HTML as a device response is precisely the kind of
      // confident wrong answer this product is built not to give.
      if (REDIRECT_STATUSES.includes(response.status)) {
        throw new DeviceError(
          "redirect_refused",
          `${this.endpointLabel} answered with a ${response.status} redirect. The device API never redirects, so the tower refused to follow it and read nothing.`,
          response.status,
        );
      }

      if (init.expectBinary) {
        return {
          status: response.status,
          json: null,
          bytes: response.body,
          headers: response.headers,
        };
      }

      const text = Buffer.from(
        response.body.buffer,
        response.body.byteOffset,
        response.body.byteLength,
      ).toString("utf8");
      let json: unknown = null;
      try {
        json = text.length > 0 ? JSON.parse(text) : null;
      } catch {
        json = null;
      }
      return { status: response.status, json, bytes: null, headers: response.headers };
    } catch (error) {
      if (error instanceof DeviceError) throw error;
      // A refused path or a refused address is a tower bug or a
      // misconfiguration. It must not be dressed up as a device that is away.
      if (error instanceof DeviceAddressError) throw error;

      if (error instanceof DeviceTransportTimeout) {
        // The deadline, and the answer is "we do not know". Unchanged from the
        // fetch implementation, which reported an AbortError the same way, and
        // it is what stops a push whose outcome nobody observed being retried.
        throw new DeviceUncertainError(error.message);
      }

      if (error instanceof DeviceTransportError) {
        throw new DeviceError(
          "unreachable",
          describeTransportFailure(error, this.endpointLabel),
          null,
          // `beforeSend` now means what its comment always said: only a
          // connection that was never established proves nothing was sent.
          // The fetch implementation claimed it for every failure, including a
          // reset that happened halfway through a 30 000-byte frame.
          error.phase === "connect",
          {
            fault: "network",
            phase: error.phase,
            errno: error.errno,
            syscall: error.syscall,
            requestSent: error.requestSent,
            connected: error.phase !== "connect",
            answered: error.phase === "response",
            heldLocally: error.heldLocally,
            endpoint: this.endpointLabel,
            elapsedMs: error.elapsedMs,
          },
        );
      }

      // Anything else reaching here is not the device. It is this process: an
      // unexpected throw from inside the transport, an instrumented module, a
      // bug in the lines above. This is the branch the outage lived in —
      // something in the server runtime threw, and the tower reported a
      // sleeping panel. The code stays `unreachable`, deliberately, so the
      // frame is still held and no push semantics move; what changes is that
      // the sentence names the cause and `fault: "tower"` says out loud that
      // the device was never asked.
      throw new DeviceError(
        "unreachable",
        `The tower's own device transport failed before it could reach ${this.endpointLabel}: ${
          error instanceof Error ? error.message : "unknown error"
        }`,
        null,
        true,
        {
          fault: "tower",
          phase: "connect",
          errno: null,
          syscall: null,
          requestSent: false,
          connected: false,
          answered: false,
          // A throw from inside this process is not a routing verdict, and a
          // retry cannot outlast it. Only the kernel's hold-down is worth
          // waiting out; see `status`.
          heldLocally: false,
          endpoint: this.endpointLabel,
          elapsedMs: 0,
        },
      );
    }
  }

  /**
   * The device's refusal codes, in words a person can act on.
   *
   * The v1 routes send a `detail` sentence and it is used as is. The v2 config
   * and action routes send a code and a field name and nothing else, because
   * every byte of a response is built on a stack buffer on the httpd task. The
   * sentences belong somewhere, and this is the boundary where the tower stops
   * speaking the wire's language.
   */
  private static readonly CODE_COPY: Record<string, string> = {
    unknown_field: "The device does not have a setting by that name",
    wrong_type: "The device expected a different type for that setting",
    out_of_range: "That value is outside the range the device accepts",
    duplicate_field: "The same setting was sent twice in one write",
    empty_patch: "The write carried no settings",
    not_object: "The device could not parse the request body",
    no_set_object: "The write carried no set object",
    missing_expected_revision:
      "The write carried no expected revision, so the device refused it",
    revision_mismatch:
      "The device configuration changed since it was read. Nothing was written.",
    missing_confirmation:
      "That change needs a typed confirmation the device did not receive",
    bad_confirmation: "The confirmation the device received did not match",
    lockdown_is_one_way:
      "Legacy writes can be blocked from here but never unblocked. That takes a button press on the device.",
    hub_url_needs_token:
      "A hub URL with no token configures nothing. Write the URL and token together first.",
    bad_hub_url: "The device refused that hub URL",
    value_too_long: "That value is longer than the device accepts",
    missing_idempotency_key:
      "The device requires an idempotency key so a retry cannot act twice",
    no_runner: "The device is not able to perform that action right now",
  };

  private static errorFrom(status: number, json: unknown): DeviceError {
    const body = json as { error?: unknown; detail?: unknown } | null;
    const code = typeof body?.error === "string" ? body.error : `http_${status}`;
    const detail =
      typeof body?.detail === "string"
        ? body.detail
        : (DeviceClient.CODE_COPY[code] ?? `Device returned ${status} (${code})`);
    return new DeviceError(code, detail, status);
  }

  /** One status read, under the device mutex. No retries, no policy. */
  private async statusOnce(): Promise<DeviceStatus> {
    return DeviceClient.single(async () => {
      const result = await this.request("/api/v1/dashboard/status", {
        method: "GET",
      });
      if (result.status !== 200) {
        throw DeviceClient.errorFrom(result.status, result.json);
      }
      const parsed = DeviceStatusSchema.safeParse(result.json);
      if (!parsed.success) {
        throw new DeviceError(
          "bad_status_shape",
          "The device status did not match the v1 contract",
          result.status,
        );
      }
      return parsed.data;
    });
  }

  /**
   * Read the device's status, retrying only what is worth retrying.
   *
   * This is the one call in the client that may happen more than once per
   * intent, and the licence is narrow and deliberate: `GET
   * /api/v1/dashboard/status` changes nothing on the device, and no write path
   * goes through here. A frame, a config patch and an action are still exactly
   * one attempt each, forever — see `putFrame` and the note on
   * DeviceUncertainError.
   *
   * What is retried is narrower still. Not "any transient error": only a
   * failure this machine produced out of its own routing table without sending
   * a packet (`heldLocally`). Those expire on a timer — twenty seconds on the
   * production host — so waiting is the entire remedy, and every other failure
   * is returned immediately, including a device that simply is not answering,
   * which fails slowly because ARP is actually attempted.
   *
   * Each attempt takes the device mutex on its own and gives it back while the
   * backoff runs, so a long manual read cannot park the scheduler behind it.
   */
  async status(options: { retryBudgetMs?: number } = {}): Promise<DeviceStatus> {
    const budgetMs = options.retryBudgetMs ?? STATUS_RETRY_BUDGET_MS;
    // The budget is spent on *waiting*, not on wall clock: a held-locally
    // attempt costs a millisecond or two, and counting those would make the
    // number mean something different on a slower machine.
    let waited = 0;
    let attempt = 0;

    for (;;) {
      try {
        return await this.statusOnce();
      } catch (error) {
        const heldLocally =
          error instanceof DeviceError && error.transport?.heldLocally === true;
        const wait = STATUS_RETRY_BACKOFF_MS[attempt];
        attempt += 1;
        if (!heldLocally || wait === undefined || waited + wait > budgetMs) {
          throw error;
        }
        waited += wait;
        await new Promise<void>((resolve) => setTimeout(resolve, wait));
      }
    }
  }

  /**
   * Push one frame. Exactly one outbound write per intent, ever. A PUT whose
   * outcome is unknown is never retried automatically: that is what the
   * uncertain state exists for.
   */
  async putFrame(
    bytes: Uint8Array,
    options: {
      sha256: string;
      idempotencyKey: string;
      epochSeconds?: number;
      /**
       * A shorter per-attempt deadline than the client's frame-upload default.
       * The automatic delivery path passes one so a write that hangs mid-wake
       * is abandoned with time to spare for another attempt in the same wake,
       * rather than burning the whole window on one sixty-second upload.
       */
      timeoutMs?: number;
    },
  ): Promise<FramePutResponse> {
    if (bytes.length !== PACKED_BYTES) {
      throw new DeviceError(
        "bad_length",
        `A frame must be exactly ${PACKED_BYTES} bytes`,
        null,
        true,
      );
    }

    return DeviceClient.single(async () => {
      const result = await this.request("/api/v1/dashboard/frame", {
        method: "PUT",
        auth: true,
        body: bytes,
        timeoutMs: options.timeoutMs ?? this.frameUploadTimeoutMs,
        headers: {
          "content-type": "application/octet-stream",
          "content-length": String(bytes.length),
          "x-frame-sha256": options.sha256,
          "idempotency-key": options.idempotencyKey,
          "x-frame-epoch": String(
            options.epochSeconds ?? Math.floor(Date.now() / 1000),
          ),
        },
      });

      if (result.status !== 200 && result.status !== 202) {
        throw DeviceClient.errorFrom(result.status, result.json);
      }
      const parsed = FramePutResponseSchema.safeParse(result.json);
      if (!parsed.success) {
        throw new DeviceError(
          "bad_put_shape",
          "The device accept response did not match the v1 contract",
          result.status,
        );
      }
      return parsed.data;
    });
  }

  async getFrame(): Promise<{ bytes: Uint8Array; sha256: string | null }> {
    return DeviceClient.single(async () => {
      const result = await this.request("/api/v1/dashboard/frame", {
        method: "GET",
        expectBinary: true,
      });
      if (result.status !== 200 || !result.bytes) {
        throw new DeviceError("not_found", "No frame stored", result.status);
      }
      return {
        bytes: result.bytes,
        sha256: result.headers.get("x-frame-sha256"),
      };
    });
  }

  async refresh(): Promise<{ accepted: boolean; render: string }> {
    return DeviceClient.single(async () => {
      const result = await this.request("/api/v1/dashboard/refresh", {
        method: "POST",
        auth: true,
        headers: { "content-length": "0" },
      });
      if (result.status !== 200 && result.status !== 202) {
        throw DeviceClient.errorFrom(result.status, result.json);
      }
      const body = result.json as { accepted?: boolean; render?: string };
      return { accepted: body?.accepted ?? true, render: body?.render ?? "queued" };
    });
  }

  /**
   * Configure the voice hub. Write only: the device never echoes the token
   * back, and neither does this method.
   */
  async setVoiceHub(
    url: string,
    token: string,
  ): Promise<{ accepted: boolean; url: string | null; tokenSet: boolean }> {
    const body = JSON.stringify({ url, token });
    return DeviceClient.single(async () => {
      const result = await this.request("/api/v1/voice/hub", {
        method: "POST",
        auth: true,
        body,
        headers: {
          "content-type": "application/json",
          "content-length": String(Buffer.byteLength(body)),
        },
      });
      if (result.status !== 200 && result.status !== 202) {
        throw DeviceClient.errorFrom(result.status, result.json);
      }
      const parsed = result.json as {
        accepted?: boolean;
        url?: string | null;
        token_set?: boolean;
        configured?: boolean;
        revision?: number;
      };
      return {
        accepted: parsed?.accepted ?? true,
        url: parsed?.url ?? null,
        // The firmware reports the flag as token_set and also reports whether
        // the pair as a whole is configured. Either one being true means the
        // device is holding a token; neither carries its value.
        tokenSet: parsed?.token_set ?? parsed?.configured ?? false,
        revision: typeof parsed?.revision === "number" ? parsed.revision : null,
      };
    });
  }

  // ------------------------------------------------------------- api 2 ----

  /** Read the whole configuration. Never returns a secret: see the schema. */
  async getConfig(): Promise<DeviceConfig> {
    return DeviceClient.single(async () => {
      const result = await this.request("/api/v1/config", {
        method: "GET",
        auth: true,
      });
      if (result.status !== 200) throw DeviceClient.errorFrom(result.status, result.json);
      const parsed = DeviceConfigSchema.safeParse(result.json);
      if (!parsed.success) {
        throw new DeviceError(
          "bad_config_shape",
          "The device configuration did not match the v2 contract",
          result.status,
        );
      }
      return parsed.data;
    });
  }

  /**
   * Write an explicit set of fields, guarded by the revision the tower read.
   *
   * There is no force and no retry. A 409 means the device changed under us
   * and the user has to see the difference before deciding; silently re-sending
   * with the device's current revision is exactly the blind overwrite the
   * compare-and-swap exists to prevent.
   */
  async patchConfig(
    expectedRevision: number,
    set: ConfigPatchValues,
    confirm?: string,
  ): Promise<ConfigPatchResponse> {
    const payload: Record<string, unknown> = {
      expected_revision: expectedRevision,
      set,
    };
    if (confirm !== undefined) payload.confirm = confirm;
    const body = JSON.stringify(payload);

    return DeviceClient.single(async () => {
      const result = await this.request("/api/v1/config", {
        method: "PATCH",
        auth: true,
        body,
        headers: {
          "content-type": "application/json",
          "content-length": String(Buffer.byteLength(body)),
        },
      });

      if (result.status === 409) {
        const conflict = result.json as { revision?: unknown } | null;
        throw new DeviceRevisionConflict(
          typeof conflict?.revision === "number" ? conflict.revision : null,
        );
      }
      if (result.status !== 200) {
        const failed = (result.json as { field?: unknown } | null)?.field;
        const error = DeviceClient.errorFrom(result.status, result.json);
        if (typeof failed === "string" && failed.length > 0) {
          throw new DeviceError(
            error.code,
            `${error.message} (field ${failed})`,
            result.status,
          );
        }
        throw error;
      }

      const parsed = ConfigPatchResponseSchema.safeParse(result.json);
      if (!parsed.success) {
        throw new DeviceError(
          "bad_patch_shape",
          "The device write response did not match the v2 contract",
          result.status,
        );
      }
      return parsed.data;
    });
  }

  /**
   * Ask the device to restart or sleep.
   *
   * The Idempotency-Key is required by the firmware, not optional politeness:
   * without one a retry after a timeout would be a second reboot.
   */
  async runAction(
    action: "restart" | "sleep",
    idempotencyKey: string,
  ): Promise<{ action: string; scheduled: boolean; replay: boolean; atMs: number }> {
    const body = JSON.stringify({ confirm: action });
    const path = action === "restart"
      ? "/api/v1/actions/restart"
      : "/api/v1/actions/sleep";

    return DeviceClient.single(async () => {
      const result = await this.request(path, {
        method: "POST",
        auth: true,
        body,
        headers: {
          "content-type": "application/json",
          "content-length": String(Buffer.byteLength(body)),
          "idempotency-key": idempotencyKey,
        },
      });
      if (result.status !== 200 && result.status !== 202) {
        throw DeviceClient.errorFrom(result.status, result.json);
      }
      const parsed = result.json as {
        action?: string;
        scheduled?: boolean;
        replay?: boolean;
        at_ms?: number;
      } | null;
      return {
        action: parsed?.action ?? action,
        scheduled: parsed?.scheduled ?? false,
        replay: parsed?.replay ?? false,
        atMs: parsed?.at_ms ?? 0,
      };
    });
  }
}

export { DeviceAddressError };
