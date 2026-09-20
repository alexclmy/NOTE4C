import http from "node:http";
import { createHash, randomBytes, timingSafeEqual } from "node:crypto";
import type { AddressInfo } from "node:net";

/**
 * A faithful mock of the NOTE4C v1 dashboard API.
 *
 * Faithful matters more than convenient here: every honesty guarantee the
 * tower makes is only as good as the failure modes it was tested against. So
 * this implements the real auth header, the real lockout window, the real
 * idempotency ring depth, the real dedup rule, the real 409 single-flight
 * discipline, and the real gap between "stored" and "displayed".
 *
 * Contract source: note4c-firmware/upstream/firmware/docs/DASHBOARD_API.md and
 * the dashboard_api.cc route table.
 */

export const FRAME_BYTES = 30_000;
export const IDEMPOTENCY_RING_DEPTH = 8;
export const AUTH_FAILURE_LIMIT = 10;
export const AUTH_WINDOW_MS = 60_000;
/** The device answers, then acts. Matches ActionGate::kDelayMs. */
export const ACTION_DELAY_MS = 1_000;
export const LAN_SERVICE_OFF_CONFIRMATION = "lan_service_off";

/**
 * The firmware's allowlist, reproduced field for field.
 *
 * Reproduced rather than imported, deliberately. The point of a mock is to be
 * a second opinion: if it shared a table with the client it is checking, a
 * field the tower spelled wrongly would be spelled wrongly in both and the
 * test would pass.
 */
export type ConfigFieldKind = "integer" | "boolean" | "string";
export const CONFIG_FIELDS: Record<
  string,
  { kind: ConfigFieldKind; apply: "immediate" | "immediate_not_persisted" | "restart_required" }
> = {
  "gallery.slide_min": { kind: "integer", apply: "immediate" },
  "sync.sync_interval": { kind: "integer", apply: "immediate" },
  "voice.muted": { kind: "boolean", apply: "immediate" },
  "voice.hub_url": { kind: "string", apply: "immediate" },
  "dashboard.lockdown": { kind: "boolean", apply: "immediate" },
  "network.lan_service": {
    kind: "boolean",
    apply: "immediate_not_persisted",
  },
  // The hybrid low-power contract. All three take effect now and are persisted.
  "power.mode": { kind: "string", apply: "immediate" },
  "power.interactive_min": { kind: "integer", apply: "immediate" },
  "power.wake_interval_min": { kind: "integer", apply: "immediate" },
};

export const DEFAULT_CAPABILITIES = [
  "dashboard.frame.v1",
  "dashboard.refresh.v1",
  "dashboard.pair.v1",
  "config.v2",
  "action.restart",
  "action.sleep",
  "power.hybrid.v1",
  "voice.hub.v1",
] as const;

export interface MockConfig {
  gallery: { slide_min: number };
  sync: { sync_interval: number };
  voice: { muted: boolean; hub_url: string; hub_token_set: boolean };
  dashboard: { lockdown: boolean };
  network: { lan_service: boolean; wifi_writable: boolean };
  /** The base mode only, exactly as the firmware reports it. */
  power: { mode: string; interactive_min: number; wake_interval_min: number };
}

function defaultConfig(lockdown: boolean): MockConfig {
  return {
    gallery: { slide_min: 5 },
    sync: { sync_interval: 30 },
    voice: { muted: true, hub_url: "", hub_token_set: false },
    dashboard: { lockdown },
    network: { lan_service: true, wifi_writable: false },
    power: { mode: "auto_saver", interactive_min: 15, wake_interval_min: 60 },
  };
}

export type RenderState = "started" | "queued" | "coalesced" | "skipped";
export type RefreshState = "idle" | "rendering" | "pending";

export interface MockDeviceOptions {
  /** 64 lowercase hex. null means the device was never paired. */
  token?: string | null;
  lockdown?: boolean;
  /** How long a simulated panel refresh takes. The real panel is ~26.5 s. */
  panelDelayMs?: number;
  /** Simulate a BUSY-pin timeout: the refresh runs but is never acknowledged. */
  failAck?: boolean;
  firmware?: string;
  api?: number;
  /**
   * Capability list. Pass a shorter one to reproduce a build that compiled
   * without a feature, which is the case the tower must render differently
   * from a feature that is merely switched off.
   */
  capabilities?: readonly string[];
  /** Open a physical pairing window so POST /pair can claim a token. */
  pairingWindowOpen?: boolean;
}

interface FrameRecord {
  present: boolean;
  seq: number;
  sha256: string;
  source_epoch: number;
  bytes: Uint8Array | null;
}

interface RingEntry {
  key: string;
  status: number;
  body: Record<string, unknown>;
}

const ERROR_STATUS: Record<string, number> = {
  unauthorized: 401,
  not_provisioned: 503,
  locked_out: 429,
  sha_mismatch: 422,
  bad_length: 400,
  too_large: 413,
  busy: 409,
  lockdown: 403,
  not_found: 404,
};

function sha256Hex(bytes: Uint8Array | Buffer): string {
  return createHash("sha256").update(bytes).digest("hex");
}

function constantTimeEquals(a: string, b: string): boolean {
  const da = createHash("sha256").update(a, "utf8").digest();
  const db = createHash("sha256").update(b, "utf8").digest();
  return timingSafeEqual(da, db);
}

export function mintToken(): string {
  return randomBytes(32).toString("hex");
}

export class MockDevice {
  private server: http.Server | null = null;
  private port = 0;

  token: string | null;
  lockdown: boolean;
  panelDelayMs: number;
  failAck: boolean;
  readonly firmware: string;
  readonly api: number;
  readonly capabilities: readonly string[];
  pairingWindowOpen: boolean;

  config: MockConfig = defaultConfig(true);
  /**
   * Monotonic, and bumped by every change however it arrived. The mock offers
   * bumpRevisionLocally() so a test can reproduce somebody pressing a button
   * on the device between the tower's read and its write.
   */
  configRevision = 0;
  /** Actions the mock would have performed. It never exits the process. */
  readonly performedActions: Array<{ action: string; atMs: number }> = [];
  private actionRing: Array<{ key: string; body: Record<string, unknown> }> = [];

  private stored: FrameRecord = {
    present: false,
    seq: 0,
    sha256: "",
    source_epoch: 0,
    bytes: null,
  };
  private displayed = { present: false, seq: 0, sha256: "" };
  private refresh = {
    state: "idle" as RefreshState,
    pending: false,
    renders: 0,
    skipped: 0,
    coalesced: 0,
  };
  private timing = { read: 3, blit: 2, panel: 0, total: 0 };
  private storage = {
    write_failures: 0,
    read_failures: 0,
    spiffs_total: 7_929_856,
    spiffs_used: 131_072,
  };

  private authFailures: number[] = [];
  private ring: RingEntry[] = [];
  private mutating = false;
  private refreshTimer: NodeJS.Timeout | null = null;
  private voiceHub: { url: string | null; tokenSet: boolean } = {
    url: null,
    tokenSet: false,
  };

  /**
   * When an interactive window closes, or null when none is open.
   *
   * Live state with a deadline, exactly as on the device: it is reported on
   * the status route and never in the configuration, and it is not persisted.
   */
  powerWindowEndsAtMs: number | null = null;
  /**
   * Whether the mock answers at all.
   *
   * The single most important thing this mock can simulate for the hybrid
   * feature, because the tower's whole design turns on a device that is
   * unreachable most of the time. `asleep = true` makes every request fail the
   * way a deep-sleeping device does: nothing listening, no answer.
   */
  asleep = false;
  /**
   * How long a request is held open before anything is answered, or 0.
   *
   * The failure `asleep` cannot reproduce, and the one production was actually
   * in. A panel with its radio off resets the socket, which fails in a
   * millisecond; a panel behind a firewall that drops packets — or a host whose
   * macOS Local Network permission has not been granted to the binary making
   * the call — accepts nothing and answers nothing, and the caller sits there
   * until its own timeout fires. Ten seconds per read, not one millisecond.
   *
   * That difference is the whole regression: every browser test in this suite
   * ran against a device that either answered at once or failed at once, so a
   * page whose first paint waits on a device read looked instant in CI and took
   * half a minute in the field. Holding the socket is the closest a listening
   * mock can get to the real thing, and it is deliberately applied before any
   * routing, so status, config and frame all stall alike.
   */
  stallMs = 0;
  /**
   * Hold a frame PUT open for this long and then answer nothing, the way a
   * panel that drops off Wi-Fi mid-upload does — while STILL answering status.
   *
   * `stallMs` stalls every route; this stalls only the frame write, which is
   * the marginal-reachability case a battery wake actually hits: the small
   * status GET lands (the tower sees a reachable device), then the 30 kB PUT
   * hangs and the tower times out into `uncertain`. That exact sequence —
   * reachable, then a hung write — is the one the automatic delivery path has
   * to survive without wedging, and `stallMs`, which would also stall the
   * status read, cannot reproduce it.
   */
  frameStallMs = 0;
  /**
   * How the last cycle went, so the tower can see a real retry state.
   *
   * `null` is the state a freshly booted device is in: no cycle has finished,
   * so there is no outcome, and the firmware renders JSON null rather than
   * naming one. A tower that cannot handle null here would show "updated" for
   * a device that has never updated.
   */
  lastOutcome: string | null = "updated";
  consecutiveFailures = 0;
  /** Which phase the budget ran out in, or null when it did not. */
  budgetExhaustedPhase: string | null = null;

  // ---- knobs the tower's awkward states need ------------------------------
  //
  // Every one of these exists because some tower behaviour is only reachable
  // when the device says something specific, and a mock that could only ever
  // say "acknowledged, no timer armed" left those paths untestable. They are
  // plain fields rather than a setter API because a test setting three of them
  // together should read as three assignments, not three calls.

  /**
   * The mode the device has been *asked* for, or null to track `mode`.
   *
   * Split from the effective mode so a test can reproduce the one state the
   * whole intent mechanism exists for: a device that has heard a request and
   * not yet acted on it.
   */
  powerDesiredMode: string | null = null;
  /** "acknowledged" or "pending_wake", or null to derive it from the two modes. */
  powerAck: string | null = null;
  /** Whether a timer wake is armed, and the countdown to it. */
  powerTimerArmed = false;
  powerNextWakeInS = 0;
  /**
   * Unix seconds of the next wake, or null for a device whose clock has never
   * been set — which is the case the tower must render as "estimated".
   */
  powerNextWakeEpoch: number | null = null;
  /** Overrides the derived value when set. */
  powerSleepIntent: boolean | null = null;
  powerLastWakeReason = "timer";
  powerAwake = true;
  /**
   * Make actions accept-but-schedule-nothing, the way a real device does when
   * something outranks the request — a panel refresh in flight, the
   * provisioning portal, a running slideshow.
   *
   * A mock that only ever answered `scheduled: true` left the tower's handling
   * of a declined action untestable, which is how "the device acknowledged and
   * is entering deep sleep" came to be printed for a device that had done no
   * such thing.
   */
  actionsScheduleNothing = false;
  /** Whether the ADC calibration exists. False reproduces an uncalibrated chip. */
  batteryCalibrated = true;
  batteryMillivolts = 3912;
  batteryPercent = 57;
  chargeState = "no_power";

  /** Every request the mock served, for assertions. Never contains a token. */
  readonly requestLog: Array<{ method: string; path: string; status: number }> = [];

  constructor(options: MockDeviceOptions = {}) {
    this.token = options.token === undefined ? mintToken() : options.token;
    this.lockdown = options.lockdown ?? true;
    this.panelDelayMs = options.panelDelayMs ?? 3_000;
    this.failAck = options.failAck ?? false;
    this.firmware = options.firmware ?? "NOTE4C-mock 0.2";
    this.api = options.api ?? 2;
    this.capabilities = options.capabilities ?? DEFAULT_CAPABILITIES;
    this.pairingWindowOpen = options.pairingWindowOpen ?? false;
    this.config = defaultConfig(this.lockdown);
  }

  /**
   * Change a setting the way the device's own Settings menu would.
   *
   * The revision moves, and the tower never saw it move. This is the only way
   * to reproduce the lost update the compare-and-swap exists to prevent.
   */
  bumpRevisionLocally(mutate?: (config: MockConfig) => void): number {
    mutate?.(this.config);
    this.configRevision += 1;
    this.lockdown = this.config.dashboard.lockdown;
    return this.configRevision;
  }

  async listen(port = 0, host = "127.0.0.1"): Promise<string> {
    this.server = http.createServer((request, response) => {
      this.handle(request, response).catch(() => {
        response.writeHead(500, { "content-type": "application/json" });
        response.end(JSON.stringify({ error: "internal", detail: "mock failure" }));
      });
    });
    await new Promise<void>((resolve) => {
      this.server?.listen(port, host, resolve);
    });
    this.port = (this.server.address() as AddressInfo).port;
    return this.origin;
  }

  get origin(): string {
    return `http://127.0.0.1:${this.port}`;
  }

  async close(): Promise<void> {
    if (this.refreshTimer) {
      clearTimeout(this.refreshTimer);
      this.refreshTimer = null;
    }
    const server = this.server;
    this.server = null;
    if (!server) return;
    await new Promise<void>((resolve) => {
      server.closeAllConnections?.();
      server.close(() => resolve());
    });
  }

  /**
   * Return the device to its just-provisioned state. Used between browser
   * suite projects so each one starts against a device that has never been
   * written to, matching the freshly reset tower store.
   */
  reset(): void {
    if (this.refreshTimer) {
      clearTimeout(this.refreshTimer);
      this.refreshTimer = null;
    }
    this.stored = {
      present: false,
      seq: 0,
      sha256: "",
      source_epoch: 0,
      bytes: null,
    };
    this.displayed = { present: false, seq: 0, sha256: "" };
    this.refresh = {
      state: "idle",
      pending: false,
      renders: 0,
      skipped: 0,
      coalesced: 0,
    };
    this.timing = { read: 3, blit: 2, panel: 0, total: 0 };
    this.authFailures = [];
    this.ring = [];
    this.actionRing = [];
    this.mutating = false;
    this.voiceHub = { url: null, tokenSet: false };
    this.lockdown = true;
    this.config = defaultConfig(true);
    this.configRevision = 0;
    this.performedActions.length = 0;
    this.requestLog.length = 0;
    // A stall left set would outlive the spec that asked for it and time out
    // every read in the next one. `asleep` is deliberately not cleared here —
    // see its note — but a stall is a transport condition, not a device state.
    this.stallMs = 0;
    this.frameStallMs = 0;
  }

  /** Test hook: current stored and displayed digests. */
  snapshot(): {
    stored: { seq: number; sha256: string };
    displayed: { seq: number; sha256: string };
    refresh: typeof MockDevice.prototype.refresh;
  } {
    return {
      stored: { seq: this.stored.seq, sha256: this.stored.sha256 },
      displayed: { seq: this.displayed.seq, sha256: this.displayed.sha256 },
      refresh: { ...this.refresh },
    };
  }

  private fail(
    response: http.ServerResponse,
    code: string,
    detail: string,
    path: string,
    method: string,
  ): void {
    const status = ERROR_STATUS[code] ?? 400;
    this.requestLog.push({ method, path, status });
    response.writeHead(status, { "content-type": "application/json" });
    response.end(JSON.stringify({ error: code, detail }));
  }

  private send(
    response: http.ServerResponse,
    status: number,
    body: unknown,
    path: string,
    method: string,
  ): void {
    this.requestLog.push({ method, path, status });
    response.writeHead(status, { "content-type": "application/json" });
    response.end(JSON.stringify(body));
  }

  /** Ten failures inside sixty seconds locks the window out entirely. */
  private lockedOut(now: number): boolean {
    this.authFailures = this.authFailures.filter(
      (at) => now - at < AUTH_WINDOW_MS,
    );
    return this.authFailures.length >= AUTH_FAILURE_LIMIT;
  }

  /** Returns an error code when auth fails, null when it passes. */
  private checkAuth(request: http.IncomingMessage): string | null {
    const now = Date.now();
    // A correct token does not bypass an active lockout.
    if (this.lockedOut(now)) return "locked_out";
    if (this.token === null) return "not_provisioned";

    const presented = request.headers["x-auth-token"];
    const value = Array.isArray(presented) ? presented[0] : presented;
    if (typeof value !== "string" || !constantTimeEquals(value, this.token)) {
      // This request is an authentication failure and is answered as one. The
      // tenth failure arms the lockout, which the next request runs into.
      this.authFailures.push(now);
      return "unauthorized";
    }
    return null;
  }

  private startRefresh(): RenderState {
    if (this.refresh.state === "rendering") {
      // Depth-1 coalescing: the first new request is pending, later ones fold
      // into it, and the pending repaint renders whatever is stored by then.
      if (this.refresh.pending) {
        this.refresh.coalesced += 1;
        return "coalesced";
      }
      this.refresh.pending = true;
      return "queued";
    }
    this.beginPanelCycle();
    return "started";
  }

  private beginPanelCycle(): void {
    this.refresh.state = "rendering";
    const startedAt = Date.now();
    this.refreshTimer = setTimeout(() => {
      this.refreshTimer = null;
      const elapsed = Date.now() - startedAt;
      this.timing.panel = elapsed;
      this.timing.total = elapsed + this.timing.read + this.timing.blit;

      if (this.failAck) {
        // The driver never signalled completion, or BUSY timed out. The panel
        // contents are genuinely unknown, so displayed keeps reporting the
        // frame still believed to be on the glass.
        this.refresh.state = "idle";
        this.refresh.pending = false;
        return;
      }

      this.refresh.renders += 1;
      this.displayed = {
        present: this.stored.present,
        seq: this.stored.seq,
        sha256: this.stored.sha256,
      };
      this.refresh.state = "idle";

      if (this.refresh.pending) {
        this.refresh.pending = false;
        this.beginPanelCycle();
      }
    }, this.panelDelayMs);
    this.refreshTimer.unref?.();
  }

  private async readBody(request: http.IncomingMessage): Promise<Buffer> {
    const chunks: Buffer[] = [];
    for await (const chunk of request) {
      chunks.push(chunk as Buffer);
    }
    return Buffer.concat(chunks);
  }

  private async handle(
    request: http.IncomingMessage,
    response: http.ServerResponse,
  ): Promise<void> {
    const method = request.method ?? "GET";
    const path = (request.url ?? "/").split("?")[0] ?? "/";

    // A deep-sleeping device has its radio off: nothing accepts the connection
    // and nothing answers. Destroying the socket is the closest a listening
    // mock can get to that, and it is what makes the tower's "asleep" path
    // testable at all. There is deliberately no request that clears this flag
    // — that is the whole point of the feature.
    if (this.asleep) {
      this.requestLog.push({ method, path, status: 0 });
      request.destroy();
      response.destroy();
      return;
    }

    // Accepted, and then nothing. See the note on `stallMs`. The timer is
    // unref'd and the socket is left to the caller's own timeout, which is
    // exactly what the tower has to survive.
    if (this.stallMs > 0) {
      this.requestLog.push({ method, path, status: 0 });
      await new Promise<void>((resolve) => {
        const timer = setTimeout(resolve, this.stallMs);
        timer.unref?.();
        request.once("aborted", () => {
          clearTimeout(timer);
          resolve();
        });
        response.once("close", () => {
          clearTimeout(timer);
          resolve();
        });
      });
      if (response.writableEnded || response.destroyed) return;
      response.destroy();
      return;
    }

    // Legacy gallery writes exist on the real device and are refused while
    // lockdown is on, which is the default. The tower must never call them.
    if (
      this.lockdown &&
      method !== "GET" &&
      ["/upload", "/photo", "/settings", "/photo/show", "/photos/move"].includes(path)
    ) {
      this.fail(response, "lockdown", "Legacy writes are blocked", path, method);
      return;
    }

    if (path === "/api/v1/dashboard/status" && method === "GET") {
      this.send(
        response,
        200,
        {
          firmware: this.firmware,
          api: this.api,
          ...(this.api >= 2
            ? {
                capabilities: [...this.capabilities],
                device: {
                  name: "NOTE4C mock panel",
                  model: "zectrix-s3-epaper-4.2",
                  hardware: "NOTE4C 4-color",
                  panel: "400x300, 4-color BWRY",
                  fw: this.firmware,
                  upstream_base: "6.5.9",
                },
                config_revision: this.configRevision,
                ...(this.capabilities.includes("power.hybrid.v1")
                  ? { power: this.powerBody() }
                  : {}),
              }
            : {}),
          initialised: true,
          provisioned: this.token !== null,
          lockdown: this.lockdown,
          stored: {
            present: this.stored.present,
            seq: this.stored.seq,
            sha256: this.stored.sha256,
            source_epoch: this.stored.source_epoch,
          },
          displayed: { ...this.displayed },
          refresh: { ...this.refresh },
          timing_ms: { ...this.timing },
          storage: { ...this.storage },
        },
        path,
        method,
      );
      return;
    }

    if (path === "/api/v1/dashboard/frame" && method === "GET") {
      if (!this.stored.present || !this.stored.bytes) {
        this.fail(response, "not_found", "No frame stored", path, method);
        return;
      }
      this.requestLog.push({ method, path, status: 200 });
      response.writeHead(200, {
        "content-type": "application/octet-stream",
        "content-length": String(FRAME_BYTES),
        "x-frame-sha256": this.stored.sha256,
      });
      response.end(Buffer.from(this.stored.bytes));
      return;
    }

    if (path === "/api/v1/dashboard/pair" && method === "POST") {
      if (!this.pairingWindowOpen) {
        this.requestLog.push({ method, path, status: 403 });
        response.writeHead(403, { "content-type": "application/json" });
        response.end(JSON.stringify({ error: "not_pairing", detail: "No pairing window is open" }));
        return;
      }
      this.token = mintToken();
      this.pairingWindowOpen = false;
      this.send(response, 200, { token: this.token }, path, method);
      return;
    }

    if (path === "/api/v1/dashboard/frame" && method === "PUT") {
      await this.handleFramePut(request, response, path, method);
      return;
    }

    if (path === "/api/v1/dashboard/refresh" && method === "POST") {
      const authError = this.checkAuth(request);
      if (authError) {
        this.fail(response, authError, "Authentication required", path, method);
        return;
      }
      if (!this.stored.present) {
        this.fail(response, "not_found", "No frame stored", path, method);
        return;
      }
      const render = this.startRefresh();
      this.send(
        response,
        render === "started" ? 202 : 200,
        { accepted: true, render },
        path,
        method,
      );
      return;
    }

    if (path === "/api/v1/voice/hub" && method === "POST") {
      const authError = this.checkAuth(request);
      if (authError) {
        this.fail(response, authError, "Authentication required", path, method);
        return;
      }
      const body = await this.readBody(request);
      let parsed: { url?: unknown; token?: unknown };
      try {
        parsed = JSON.parse(body.toString("utf8"));
      } catch {
        this.fail(response, "bad_length", "Body is not JSON", path, method);
        return;
      }
      if (typeof parsed.url === "string") {
        this.voiceHub.url = parsed.url;
        this.config.voice.hub_url = parsed.url;
      }
      if (typeof parsed.token === "string" && parsed.token.length > 0) {
        this.voiceHub.tokenSet = true;
        this.config.voice.hub_token_set = true;
      }
      // This route changes a setting GET /api/v1/config reports, so it moves
      // the same revision every other change moves.
      this.configRevision += 1;
      // Write only. The response never echoes the token back.
      this.send(
        response,
        200,
        {
          accepted: true,
          configured: Boolean(this.voiceHub.url),
          url: this.voiceHub.url,
          token_set: this.voiceHub.tokenSet,
          revision: this.configRevision,
        },
        path,
        method,
      );
      return;
    }

    if (path === "/api/v1/config" && this.api >= 2) {
      if (method === "GET") {
        const authError = this.checkAuth(request);
        if (authError) {
          this.fail(response, authError, "Authentication required", path, method);
          return;
        }
        this.send(response, 200, this.configBody(), path, method);
        return;
      }
      if (method === "PATCH") {
        await this.handleConfigPatch(request, response, path, method);
        return;
      }
    }

    if (
      (path === "/api/v1/actions/restart" || path === "/api/v1/actions/sleep") &&
      method === "POST" &&
      this.api >= 2
    ) {
      await this.handleAction(
        request,
        response,
        path,
        method,
        path.endsWith("restart") ? "restart" : "sleep",
      );
      return;
    }

    this.fail(response, "not_found", "No such route", path, method);
  }

  /**
   * The `power` object the firmware renders from common/power_policy.cc.
   *
   * Faithful in the one way that matters most to the tower: an uncalibrated or
   * implausible battery reports null for both numbers rather than a plausible
   * figure, so a tower bug that renders a made-up percentage fails against the
   * mock rather than in the field.
   */
  powerBody(): Record<string, unknown> {
    const now = Date.now();
    const windowOpen =
      this.powerWindowEndsAtMs !== null && this.powerWindowEndsAtMs > now;
    const effective = windowOpen ? "interactive" : this.config.power.mode;
    const plausible =
      this.batteryMillivolts >= 2800 && this.batteryMillivolts <= 4400;
    const usable = this.batteryCalibrated && plausible && this.batteryMillivolts > 0;
    const desired = this.powerDesiredMode ?? effective;
    return {
      contract: 1,
      mode: effective,
      desired_mode: desired,
      ack: this.powerAck ?? (desired === effective ? "acknowledged" : "pending_wake"),
      awake: this.powerAwake,
      sleep_intent: this.powerSleepIntent ?? effective === "auto_saver",
      interactive_remaining_s: windowOpen
        ? Math.ceil(((this.powerWindowEndsAtMs as number) - now) / 1000)
        : 0,
      wake_interval_min: this.config.power.wake_interval_min,
      timer_armed: this.powerTimerArmed,
      next_wake_in_s: this.powerTimerArmed ? this.powerNextWakeInS : 0,
      next_wake_epoch: this.powerTimerArmed ? this.powerNextWakeEpoch : null,
      last_wake_reason: this.powerLastWakeReason,
      last_outcome: this.lastOutcome,
      budget_exhausted_phase: this.budgetExhaustedPhase,
      consecutive_failures: this.consecutiveFailures,
      battery: {
        present: this.batteryMillivolts > 0,
        calibrated: this.batteryCalibrated,
        plausible,
        mv: usable ? this.batteryMillivolts : null,
        percent: usable ? this.batteryPercent : null,
      },
      charge: {
        state: this.chargeState,
        charging: this.chargeState === "charging",
      },
    };
  }

  private configBody(): Record<string, unknown> {
    // A structural copy. There is no field here that could hold the hub token,
    // which is what makes "the config route never leaks it" more than a habit.
    return {
      api: this.api,
      revision: this.configRevision,
      config: {
        gallery: { slide_min: this.config.gallery.slide_min },
        sync: { sync_interval: this.config.sync.sync_interval },
        voice: {
          muted: this.config.voice.muted,
          hub_url: this.config.voice.hub_url,
          hub_token_set: this.config.voice.hub_token_set,
        },
        dashboard: { lockdown: this.config.dashboard.lockdown },
        network: {
          lan_service: this.config.network.lan_service,
          wifi_writable: false,
        },
        power: {
          mode: this.config.power.mode,
          interactive_min: this.config.power.interactive_min,
          wake_interval_min: this.config.power.wake_interval_min,
        },
      },
    };
  }

  private failField(
    response: http.ServerResponse,
    status: number,
    code: string,
    field: string | null,
    path: string,
    method: string,
  ): void {
    this.requestLog.push({ method, path, status });
    response.writeHead(status, { "content-type": "application/json" });
    response.end(JSON.stringify(field ? { error: code, field } : { error: code }));
  }

  /**
   * The compare-and-swap write.
   *
   * Faithful to the firmware in the ways that matter to the tower: unknown
   * names are refused by name, types are strict, the revision is checked
   * before the bounds, lockdown is one way, turning the LAN service off needs
   * a literal, and a patch that fails on any field applies none of them.
   */
  private async handleConfigPatch(
    request: http.IncomingMessage,
    response: http.ServerResponse,
    path: string,
    method: string,
  ): Promise<void> {
    const authError = this.checkAuth(request);
    if (authError) {
      this.fail(response, authError, "Authentication required", path, method);
      return;
    }

    let parsed: { expected_revision?: unknown; set?: unknown; confirm?: unknown };
    try {
      parsed = JSON.parse((await this.readBody(request)).toString("utf8"));
    } catch {
      this.failField(response, 400, "not_object", null, path, method);
      return;
    }
    if (!parsed || typeof parsed !== "object") {
      this.failField(response, 400, "not_object", null, path, method);
      return;
    }
    const set = parsed.set;
    if (!set || typeof set !== "object" || Array.isArray(set)) {
      this.failField(response, 400, "no_set_object", null, path, method);
      return;
    }

    const entries = Object.entries(set as Record<string, unknown>);
    if (entries.length === 0) {
      this.failField(response, 400, "empty_patch", null, path, method);
      return;
    }

    // Names and types first, so an unknown field is named back rather than
    // being silently ignored.
    for (const [name, value] of entries) {
      const spec = CONFIG_FIELDS[name];
      if (!spec) {
        this.failField(response, 400, "unknown_field", name, path, method);
        return;
      }
      const typeOk =
        spec.kind === "integer"
          ? typeof value === "number" && Number.isInteger(value)
          : spec.kind === "boolean"
            ? typeof value === "boolean"
            : typeof value === "string";
      if (!typeOk) {
        this.failField(response, 400, "wrong_type", name, path, method);
        return;
      }
    }

    if (typeof parsed.expected_revision !== "number") {
      this.failField(response, 400, "missing_expected_revision", null, path, method);
      return;
    }
    if (parsed.expected_revision !== this.configRevision) {
      // The current revision goes back with the refusal, so the loser of the
      // race can re-read and reconcile in one round trip.
      this.requestLog.push({ method, path, status: 409 });
      response.writeHead(409, { "content-type": "application/json" });
      response.end(
        JSON.stringify({ error: "revision_mismatch", revision: this.configRevision }),
      );
      return;
    }

    const confirm = typeof parsed.confirm === "string" ? parsed.confirm : null;
    for (const [name, value] of entries) {
      if (name === "network.lan_service" && value === false) {
        if (confirm === null) {
          this.failField(response, 400, "missing_confirmation", name, path, method);
          return;
        }
        if (confirm !== LAN_SERVICE_OFF_CONFIRMATION) {
          this.failField(response, 400, "bad_confirmation", name, path, method);
          return;
        }
      }
      if (name === "dashboard.lockdown" && value === false) {
        this.failField(response, 403, "lockdown_is_one_way", name, path, method);
        return;
      }
      if (name === "gallery.slide_min" && ![0, 5, 10, 30].includes(value as number)) {
        this.failField(response, 400, "out_of_range", name, path, method);
        return;
      }
      if (
        name === "sync.sync_interval" &&
        ((value as number) < 0 || (value as number) > 1440)
      ) {
        this.failField(response, 400, "out_of_range", name, path, method);
        return;
      }
      if (
        name === "power.mode" &&
        !["auto_saver", "interactive", "always_on"].includes(value as string)
      ) {
        this.failField(response, 400, "out_of_range", name, path, method);
        return;
      }
      if (
        name === "power.interactive_min" &&
        ![5, 15, 30, 60].includes(value as number)
      ) {
        this.failField(response, 400, "out_of_range", name, path, method);
        return;
      }
      if (
        name === "power.wake_interval_min" &&
        ((value as number) < 15 || (value as number) > 1440)
      ) {
        this.failField(response, 400, "out_of_range", name, path, method);
        return;
      }
      if (name === "voice.hub_url" && value !== "") {
        const url = value as string;
        // http or https, a host, and nothing that could change the shape of
        // the request the device would build out of it.
        if (!/^https?:\/\/[^\s\x00-\x1f"'<>]+$/.test(url)) {
          this.failField(response, 400, "bad_hub_url", name, path, method);
          return;
        }
        if (!this.config.voice.hub_token_set) {
          this.failField(response, 400, "hub_url_needs_token", name, path, method);
          return;
        }
      }
    }

    // Everything checked. Only now does anything change.
    const applied: Record<string, string> = {};
    let requestedPowerMode: string | null = null;
    for (const [name, value] of entries) {
      switch (name) {
        case "gallery.slide_min":
          this.config.gallery.slide_min = value as number;
          break;
        case "sync.sync_interval":
          this.config.sync.sync_interval = value as number;
          break;
        case "voice.muted":
          this.config.voice.muted = value as boolean;
          break;
        case "voice.hub_url":
          this.config.voice.hub_url = (value as string).replace(/\/+$/, "");
          if (this.config.voice.hub_url === "") {
            this.config.voice.hub_token_set = false;
            this.voiceHub = { url: null, tokenSet: false };
          } else {
            this.voiceHub.url = this.config.voice.hub_url;
          }
          break;
        case "dashboard.lockdown":
          this.config.dashboard.lockdown = value as boolean;
          this.lockdown = value as boolean;
          break;
        case "network.lan_service":
          this.config.network.lan_service = value as boolean;
          break;
        case "power.mode":
          // Recorded now, acted on after the loop. Opening the window here
          // would use whatever interactive_min happened to be set *before*
          // this patch, and a patch that sets the mode and the length together
          // would silently get the old length. The firmware avoids this by
          // computing the whole new config first and calling one hook with
          // both values; this is the same fix.
          requestedPowerMode = value as string;
          break;
        case "power.interactive_min":
          // The length of the *next* window. A patch that carries this and not
          // power.mode deliberately leaves powerWindowEndsAtMs alone: changing
          // how long a window lasts is not a request to be thrown out of the
          // one that is open. The firmware routes this to its own hook for the
          // same reason; see device_config_service.cc.
          this.config.power.interactive_min = value as number;
          break;
        case "power.wake_interval_min":
          this.config.power.wake_interval_min = value as number;
          break;
        default:
          break;
      }
      applied[name] = CONFIG_FIELDS[name]!.apply;
    }

    // The mode last, against the fully updated configuration, so a patch that
    // sets the mode and the window length together opens a window of the
    // length that was just written rather than the one it replaced.
    if (requestedPowerMode !== null) {
      if (requestedPowerMode === "interactive") {
        // A live window with a deadline. The *base* mode stays auto_saver,
        // because a window that came back from storage after a reboot would be
        // a window nobody opened.
        this.powerWindowEndsAtMs =
          Date.now() + this.config.power.interactive_min * 60_000;
        this.config.power.mode = "auto_saver";
      } else {
        this.powerWindowEndsAtMs = null;
        this.config.power.mode = requestedPowerMode;
      }
    }

    this.configRevision += 1;
    this.send(response, 200, { ...this.configBody(), applied }, path, method);
  }

  /**
   * Restart and sleep, which this mock records rather than performs.
   *
   * A mock that actually exited would make the idempotency test impossible to
   * write, which is the test that matters: a caller retrying after a timeout
   * must not reboot the device twice.
   */
  private async handleAction(
    request: http.IncomingMessage,
    response: http.ServerResponse,
    path: string,
    method: string,
    action: "restart" | "sleep",
  ): Promise<void> {
    const authError = this.checkAuth(request);
    if (authError) {
      this.fail(response, authError, "Authentication required", path, method);
      return;
    }

    let parsed: { confirm?: unknown };
    try {
      parsed = JSON.parse((await this.readBody(request)).toString("utf8"));
    } catch {
      this.failField(response, 400, "not_object", null, path, method);
      return;
    }
    if (typeof parsed?.confirm !== "string") {
      this.failField(response, 400, "missing_confirmation", null, path, method);
      return;
    }
    // The literal is the action's own name, so a body sent to the wrong route
    // cannot confirm the action it reached.
    if (parsed.confirm !== action) {
      this.failField(response, 400, "bad_confirmation", null, path, method);
      return;
    }

    const keyHeader = request.headers["idempotency-key"];
    const key = Array.isArray(keyHeader) ? keyHeader[0] : keyHeader;
    if (typeof key !== "string" || key.length === 0) {
      this.failField(response, 400, "missing_idempotency_key", null, path, method);
      return;
    }

    const previous = this.actionRing.find((entry) => entry.key === key);
    if (previous) {
      this.send(
        response,
        200,
        { ...previous.body, scheduled: false, replay: true },
        path,
        method,
      );
      return;
    }

    if (this.actionsScheduleNothing) {
      // Accepted and declined. Deliberately not added to performedActions: the
      // device did not perform it. Deliberately not added to the idempotency
      // ring either — nothing was scheduled, so a later identical request is a
      // fresh chance rather than a replay of a thing that happened.
      this.send(
        response,
        200,
        { action, scheduled: false, replay: false, at_ms: 0 },
        path,
        method,
      );
      return;
    }

    const body = {
      action,
      scheduled: true,
      replay: false,
      at_ms: ACTION_DELAY_MS,
    };
    this.actionRing.push({ key, body });
    while (this.actionRing.length > IDEMPOTENCY_RING_DEPTH) this.actionRing.shift();
    this.performedActions.push({ action, atMs: ACTION_DELAY_MS });
    this.send(response, 202, body, path, method);
  }

  private async handleFramePut(
    request: http.IncomingMessage,
    response: http.ServerResponse,
    path: string,
    method: string,
  ): Promise<void> {
    // Marginal reachability: the connection was accepted (status already
    // answered this wake) and now the write hangs and never comes back. Held
    // open, unref'd, then the socket is destroyed with no response, so the
    // caller pays its own upload timeout exactly as it does against a panel
    // that fell off the network mid-PUT. Counted as a status-0 attempt: nothing
    // was served.
    if (this.frameStallMs > 0) {
      this.requestLog.push({ method, path, status: 0 });
      await new Promise<void>((resolve) => {
        const timer = setTimeout(resolve, this.frameStallMs);
        timer.unref?.();
        request.once("aborted", () => {
          clearTimeout(timer);
          resolve();
        });
        response.once("close", () => {
          clearTimeout(timer);
          resolve();
        });
      });
      if (response.writableEnded || response.destroyed) return;
      response.destroy();
      return;
    }

    const authError = this.checkAuth(request);
    if (authError) {
      this.fail(response, authError, "Authentication required", path, method);
      return;
    }

    // One mutating request at a time. A second gets 409 rather than queueing,
    // so a client always knows whether its frame was applied.
    if (this.mutating) {
      this.fail(response, "busy", "Another mutating request is in flight", path, method);
      return;
    }
    this.mutating = true;
    try {
      const declared = Number(request.headers["content-length"] ?? "-1");
      if (declared > FRAME_BYTES) {
        this.fail(response, "too_large", "Content-Length exceeds 30000", path, method);
        return;
      }

      const body = await this.readBody(request);
      if (body.length !== FRAME_BYTES || declared !== FRAME_BYTES) {
        this.fail(
          response,
          "bad_length",
          `Expected exactly ${FRAME_BYTES} bytes, received ${body.length}`,
          path,
          method,
        );
        return;
      }

      const keyHeader = request.headers["idempotency-key"];
      const key = Array.isArray(keyHeader) ? keyHeader[0] : keyHeader;
      if (typeof key === "string" && key.length > 0) {
        const previous = this.ring.find((entry) => entry.key === key);
        if (previous) {
          // A repeat is not repainted. It replays the original answer.
          this.send(
            response,
            200,
            { ...previous.body, replay: true, render: "skipped" },
            path,
            method,
          );
          return;
        }
      }

      const digest = sha256Hex(body);
      const claimedHeader = request.headers["x-frame-sha256"];
      const claimed = Array.isArray(claimedHeader) ? claimedHeader[0] : claimedHeader;
      if (typeof claimed === "string" && claimed.length > 0 && claimed !== digest) {
        this.fail(
          response,
          "sha_mismatch",
          "Body does not match X-Frame-Sha256",
          path,
          method,
        );
        return;
      }

      const epochHeader = request.headers["x-frame-epoch"];
      const epochValue = Array.isArray(epochHeader) ? epochHeader[0] : epochHeader;
      const epoch = Number(epochValue ?? 0);

      if (this.stored.present && this.stored.sha256 === digest) {
        // Identical bytes are detected by digest and skipped outright. This is
        // a real saving: no repaint, no seq bump, no panel wear.
        this.refresh.skipped += 1;
        const body200 = {
          accepted: true,
          persisted: true,
          deduped: true,
          replay: false,
          seq: this.stored.seq,
          sha256: digest,
          render: "skipped" as RenderState,
        };
        this.rememberKey(key, 200, body200);
        this.send(response, 200, body200, path, method);
        return;
      }

      this.stored = {
        present: true,
        seq: this.stored.seq + 1,
        sha256: digest,
        source_epoch: Number.isFinite(epoch) ? epoch : 0,
        bytes: new Uint8Array(body),
      };
      this.storage.spiffs_used = 131_072 + FRAME_BYTES * 2;

      const render = this.startRefresh();
      const body202 = {
        accepted: true,
        persisted: true,
        deduped: false,
        replay: false,
        seq: this.stored.seq,
        sha256: digest,
        render,
      };
      this.rememberKey(key, 202, body202);
      this.send(response, 202, body202, path, method);
    } finally {
      this.mutating = false;
    }
  }

  private rememberKey(
    key: string | undefined,
    status: number,
    body: Record<string, unknown>,
  ): void {
    if (typeof key !== "string" || key.length === 0) return;
    this.ring.push({ key, status, body });
    while (this.ring.length > IDEMPOTENCY_RING_DEPTH) this.ring.shift();
  }
}
