"use client";

import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import Link from "next/link";
import { ApiError, apiGet, apiSend, isAbort } from "@/ui/api";
import {
  Badge,
  Banner,
  Button,
  Card,
  Empty,
  ErrorNote,
  FreshnessStamp,
  Row,
} from "@/ui/components";
import { ConfirmDialog } from "@/ui/Dialog";
import { Disclosure, Fold } from "@/ui/Disclosure";
import { PageLoading } from "@/ui/PageState";
import { StateStrip } from "@/ui/StateStrip";
import { StickyActions } from "@/ui/StickyActions";
import { PowerPanel } from "@/ui/PowerPanel";
import { RealDeviceCard } from "@/ui/RealDeviceCard";
import { publishDeviceStatus, toDeviceReading } from "@/ui/useDeviceState";
import { useIsNarrow } from "@/ui/useMediaQuery";
import { MODE_COPY, type DevicePower, type PowerIntent, type PowerMode } from "@/core/power";
import {
  APPLY_MODE_BADGE,
  APPLY_MODE_COPY,
  SETTINGS_REGISTRY,
  aboutFields,
  gatedTooltip,
  isEditable,
  isGated,
  isLive,
  needsConfirmation,
  negotiate,
  type ApplyMode,
  type DeviceCapabilities,
  type SettingEntry,
} from "@/core/registry";

interface StatusPayload {
  reachable: boolean;
  simulated: boolean;
  tokenConfigured: boolean;
  deviceMode: "mock" | "real";
  deviceAddress: string | null;
  readAt: string;
  detail?: string;
  status?: {
    firmware: string;
    api: number;
    capabilities?: string[];
    device?: {
      name?: string;
      model?: string;
      hardware?: string;
      panel?: string;
      fw?: string;
      upstream_base?: string;
    };
    config_revision?: number;
    provisioned: boolean;
    lockdown: boolean;
    stored: { present: boolean; seq: number; sha256: string };
    displayed: { present: boolean; seq: number; sha256: string };
    storage?: Record<string, number>;
  };
  /** The hybrid low-power block. See src/core/power.ts. */
  power?: DevicePower | null;
  powerSupported?: boolean | null;
  powerIntent?: PowerIntent | null;
  powerIntentDetail?: string;
  batteryUnavailableReason?: string | null;
  reachabilityNote?: string;
  nextWake?: { at: string; estimated: boolean } | null;
  deviceLastSeenAt?: string | null;
  /**
   * Whether the device was actually asked.
   *
   * False only on the first paint, which comes from `?observe=0` — the tower's
   * own record, no socket. Absent means true, which is what every payload from
   * this route meant before that existed.
   */
  observed?: boolean;
  /** What the device said the last time it answered, and when. */
  lastConfirmed?: {
    at: string;
    power: DevicePower | null;
    powerSupported: boolean | null;
  } | null;
  /** Present only on a failed read. See src/server/device/failure.ts. */
  failure?: {
    kind: string;
    code: string;
    errno: string | null;
    heldLocally: boolean;
    deviceAnswered: boolean | null;
  };
}

interface ConfigValues {
  gallery: { slide_min: number };
  sync: { sync_interval: number };
  voice: { muted: boolean; hub_url: string; hub_token_set: boolean };
  dashboard: { lockdown: boolean };
  network: { lan_service: boolean; wifi_writable?: boolean };
}

interface ConfigPayload {
  reachable: boolean;
  simulated: boolean;
  supported: boolean;
  detail?: string;
  revision?: number;
  config?: ConfigValues;
  readAt?: string;
}

type FieldValue = number | boolean | string;

const SECTIONS = ["System", "Gallery", "Network", "Dashboard", "Voice"] as const;

/** Read one dotted field out of the device's nested config object. */
function readField(config: ConfigValues | undefined, field: string): FieldValue | undefined {
  if (!config) return undefined;
  const [group, name] = field.split(".");
  const bucket = (config as unknown as Record<string, Record<string, FieldValue>>)[
    group ?? ""
  ];
  return bucket?.[name ?? ""];
}

function formatValue(entry: SettingEntry, value: FieldValue | undefined): string {
  if (value === undefined) return "not reported";
  if (typeof value === "boolean") return value ? "on" : "off";
  if (typeof value === "number") {
    const unit = entry.constraints?.unit;
    if (value === 0 && entry.key === "gallery.slide_min") return "off";
    if (value === 0 && entry.key === "system.sync_interval") return "never";
    return unit ? `${value} ${unit}` : String(value);
  }
  return value.length > 0 ? value : "not set";
}

/**
 * The Device page, in the order the work actually happens.
 *
 * The previous order was: power, five cards of settings, then an Apply card,
 * then the real-device connection, then About. Which meant the control that
 * writes your edits was two screens below the field you edited, and the page
 * offered to configure a device you had not connected to yet. Now:
 *
 *  1. what the device is doing (the same strip Overview shows),
 *  2. power — the thing that decides whether anything else can be written,
 *  3. the connection, because you do not configure a device you are not
 *     pointed at,
 *  4. the settings themselves,
 *  5. a bar that follows your unsaved edits down the page,
 *  6. About and Storage, folded away on a phone: evidence, not controls.
 */
export default function DevicePage() {
  const [data, setData] = useState<StatusPayload | null>(null);
  const [config, setConfig] = useState<ConfigPayload | null>(null);
  const [edits, setEdits] = useState<Record<string, FieldValue>>({});
  const [confirmWord, setConfirmWord] = useState("");
  const [applied, setApplied] = useState<Record<string, ApplyMode>>({});
  const [conflict, setConflict] = useState<string>("");
  const [error, setError] = useState("");
  const [notice, setNotice] = useState("");
  const [busy, setBusy] = useState(false);
  const [actionDialog, setActionDialog] = useState<"none" | "restart" | "sleep">(
    "none",
  );
  /** A read somebody pressed for, in flight. The button says so; see below. */
  const [checking, setChecking] = useState(false);
  const narrow = useIsNarrow();

  /**
   * Which read is allowed to win.
   *
   * Reads of this page overlap for entirely ordinary reasons — a mount, the
   * reload after applying a setting, and somebody pressing "Check now" — and
   * they do not come back in the order they were sent: a read that waits out a
   * routing hold-down takes twenty seconds, and one the kernel refuses out of
   * its own table takes one millisecond. Without a ticket, the stale answer
   * from the read started first lands last and overwrites the fresh one, so
   * the page goes back to saying the device is away a moment after proving it
   * was not. The ledger of this product's own incident has exactly that shape.
   */
  const readTicket = useRef(0);

  /**
   * Whether a read that actually asked the device has landed.
   *
   * The first paint comes from the tower's record rather than from the panel,
   * and it must never be written over the top of a real answer. `readTicket`
   * cannot decide that: `load()` takes its ticket synchronously, so by the
   * time the record read returns the ticket has already moved and every
   * comparison against it is false. What matters is not which read is newest
   * but whether anything has been *observed* yet, which is its own fact.
   */
  const observedLanded = useRef(false);

  /**
   * The reads in flight, so leaving the page gives their sockets back.
   *
   * A browser opens six connections to one origin. A device read waiting out a
   * ten-second transport timeout holds one for the whole of it, and this page
   * starts two — a status and a config, because the config route negotiates
   * capability from the device's own status before it reads anything. Two
   * visits to a page nobody is looking at any more is four sockets gone, and
   * the *next* visit's fast local read queues behind them and never arrives.
   * That is the loading failure reappearing one level down, in the browser
   * rather than in the tower, and it is what this exists to prevent.
   */
  const inFlight = useRef<AbortController | null>(null);

  /**
   * The two device reads this page needs, each landing on its own.
   *
   * They used to be `Promise.all`ed and assigned together, and that cost the
   * whole page. `GET /api/device/config` negotiates capability from the
   * device's own status before it reads anything, so it is a *second* device
   * round trip — and every device call in this process is serialised through
   * one mutex, so the two are not concurrent, they are consecutive. Against a
   * panel that accepts a socket and never answers, which is what a dropped
   * packet or a missing macOS Local Network permission looks like, that is two
   * ten-second timeouts before `setData` ran once. With the shell's own read
   * ahead of them it was three, and the page held its skeleton for all of it.
   *
   * Settled rather than raced: a config read that fails must not blank the
   * status the page just got, and neither may leave "Checking…" on the button
   * forever. The ticket still decides which read is allowed to win.
   */
  const load = useCallback(async (manual = false) => {
    const ticket = (readTicket.current += 1);
    inFlight.current?.abort();
    const controller = new AbortController();
    inFlight.current = controller;
    if (manual) setChecking(true);

    const status = apiGet<StatusPayload>(
      // `force=1` on a read a person asked for: no cache anywhere in the
      // path, and the server is allowed to spend real time getting a true
      // answer rather than a fast wrong one. See the status route.
      `/api/device/status${manual ? "?force=1" : ""}`,
      { signal: controller.signal },
    ).then(
      (payload) => {
        if (ticket !== readTicket.current) return;
        observedLanded.current = true;
        setData(payload);
        publishDeviceStatus(payload);
        setError("");
      },
      (caught: unknown) => {
        if (ticket !== readTicket.current || isAbort(caught)) return;
        setError(
          caught instanceof ApiError ? caught.message : "The tower did not answer",
        );
      },
    );

    const config = apiGet<ConfigPayload>("/api/device/config", {
      signal: controller.signal,
    }).then(
      (payload) => {
        if (ticket !== readTicket.current) return;
        setConfig(payload);
        // A fresh read discards pending edits on purpose. Keeping them would
        // mean the form showed a value the user typed next to a revision that
        // no longer matches it.
        setEdits({});
        setConfirmWord("");
      },
      () => {
        // The settings block reports its own absence — `supported: false` and
        // an explanation — and a status read that succeeded is still worth
        // showing. Nothing to raise here that the page does not already say.
      },
    );

    await Promise.allSettled([status, config]);
    if (inFlight.current === controller) inFlight.current = null;
    if (ticket === readTicket.current) setChecking(false);
  }, []);

  /**
   * Paint first, read second.
   *
   * `?observe=0` is the tower's own record of the device with no socket
   * opened: what it last said, when, what is queued for it, which address it
   * is at. It comes back in under a millisecond however dead the panel is, so
   * the page has its state card, its controls and its evidence on screen
   * before the read that confirms them has finished — and that read then
   * replaces it through the same ordering every other read goes through.
   *
   * It is deliberately not `Promise.all`ed with `load()`: the whole point is
   * that it lands first. `observed: false` on the payload is what keeps the
   * interface from reading it as a failed read; see src/core/power.ts.
   */
  useEffect(() => {
    const paint = new AbortController();
    void (async () => {
      try {
        const known = await apiGet<StatusPayload>("/api/device/status?observe=0", {
          signal: paint.signal,
        });
        // Only if the real read has not already beaten it home. Not a mount
        // flag: the record read and the device read are ordered by *which
        // asked the device*, not by which was started first.
        if (!observedLanded.current) {
          setData(known);
          publishDeviceStatus(known);
        }
      } catch {
        // Nothing to say: `load()` below is about to report for itself.
      }
    })();
    void load();
    return () => {
      paint.abort();
      inFlight.current?.abort();
    };
  }, [load]);

  const device: DeviceCapabilities = useMemo(
    () => negotiate(data?.status ?? { api: 1 }),
    [data],
  );

  const pending = Object.entries(edits);
  const confirmEntry = pending
    .map(([field, value]) => {
      const entry = SETTINGS_REGISTRY.find((row) => row.field === field);
      return entry && needsConfirmation(entry, value) ? entry : null;
    })
    .find((entry): entry is SettingEntry => entry !== null);

  function setEdit(entry: SettingEntry, value: FieldValue): void {
    if (!entry.field) return;
    const current = readField(config?.config, entry.field);
    setEdits((previous) => {
      const next = { ...previous };
      if (value === current) delete next[entry.field as string];
      else next[entry.field as string] = value;
      return next;
    });
    setNotice("");
  }

  async function apply(): Promise<void> {
    if (pending.length === 0 || config?.revision === undefined) return;
    setBusy(true);
    setError("");
    setNotice("");
    setConflict("");
    try {
      const result = await apiSend<{
        revision: number;
        config: ConfigValues;
        applied: Record<string, ApplyMode>;
      }>("/api/device/config", "PATCH", {
        expectedRevision: config.revision,
        set: edits,
        confirm: confirmWord.length > 0 ? confirmWord : undefined,
      });
      setConfig({ ...config, revision: result.revision, config: result.config });
      setApplied(result.applied ?? {});
      setEdits({});
      setConfirmWord("");
      setNotice(
        `The device confirmed ${pending.length} change${pending.length === 1 ? "" : "s"} at revision ${result.revision}.`,
      );
      // Re-read the status so the About block's revision matches. Marked
      // observed for the same reason the mount read is: whatever this returns,
      // the device was asked, and the first-paint record must never land on
      // top of it afterwards.
      const status = await apiGet<StatusPayload>("/api/device/status");
      observedLanded.current = true;
      setData(status);
      publishDeviceStatus(status);
    } catch (caught) {
      if (caught instanceof ApiError && caught.status === 409) {
        // Never a blind retry. The edits are kept so the user can compare them
        // against what the device now says, and choose.
        setConflict(caught.message);
        try {
          const fresh = await apiGet<ConfigPayload>("/api/device/config");
          setConfig(fresh);
        } catch {
          setConflict(
            `${caught.message} The tower also could not re-read the device.`,
          );
        }
      } else {
        setError(caught instanceof ApiError ? caught.message : "The write failed");
      }
    } finally {
      setBusy(false);
    }
  }

  /**
   * Send a power request.
   *
   * The response distinguishes applied from pending, and this reports the
   * difference rather than flattening both into "done". A user who is told
   * "done" about a sleeping device will believe it.
   */
  async function sendPower(
    body: Record<string, unknown>,
    optimisticLabel: string,
  ): Promise<void> {
    setBusy(true);
    setError("");
    setNotice("");
    try {
      const result = await apiSend<{
        applied?: boolean;
        pending?: boolean;
        detail?: string;
        nextWake?: { at: string; estimated: boolean } | null;
      }>("/api/device/power", "POST", body);
      if (result.applied) {
        setNotice(`${optimisticLabel}: applied to the device.`);
      } else if (result.pending) {
        const when = result.nextWake
          ? ` The device is next expected around ${new Date(result.nextWake.at).toLocaleTimeString()}${result.nextWake.estimated ? " (estimated)" : ""}.`
          : "";
        setNotice(`${optimisticLabel}: held until the device next wakes.${when} ${result.detail ?? ""}`);
      } else {
        // Neither applied nor pending: a cancel, a decline, or a reconcile with
        // nothing to do. Every one of those answers with a `detail`, but fall
        // back to the label rather than to `undefined` — a click that produces
        // no visible response is indistinguishable from a broken button, which
        // is exactly what cancelling a pending request used to look like.
        setNotice(result.detail ?? `${optimisticLabel}: done.`);
      }
      await load();
    } catch (caught) {
      setError(caught instanceof ApiError ? caught.message : "The tower did not answer");
    } finally {
      setBusy(false);
    }
  }

  async function runAction(action: "restart" | "sleep"): Promise<void> {
    const entry = SETTINGS_REGISTRY.find((row) => row.key === `system.${action}`);
    setBusy(true);
    setError("");
    setNotice("");
    try {
      const result = await apiSend<{ replay: boolean; atMs: number }>(
        "/api/device/actions",
        "POST",
        {
          action,
          confirm: entry?.confirmGate,
          // One key per intent, so a retry of this click is the same request.
          idempotencyKey: `${action}-${Date.now()}`,
        },
      );
      setNotice(
        result.replay
          ? "The device had already accepted this request and did nothing further."
          : `The device accepted it and acts in ${result.atMs} ms.`,
      );
    } catch (caught) {
      setError(caught instanceof ApiError ? caught.message : "The action failed");
    } finally {
      setBusy(false);
      setActionDialog("none");
    }
  }

  function renderControl(entry: SettingEntry): React.ReactNode {
    const field = entry.field;
    if (!field) return null;
    const stored = readField(config?.config, field);
    const value = edits[field] ?? stored;
    const dirty = field in edits;
    const testId = `control-${entry.key}`;

    if (entry.type === "boolean") {
      // A one-way flag that is already in its final state has no control worth
      // offering. Saying why, inline, beats a control that looks live and
      // would be refused: the reason is the interesting part.
      const settled = entry.oneWayTo !== undefined && stored === entry.oneWayTo;
      return (
        <label className={dirty ? "inline-edit dirty" : "inline-edit"}>
          <input
            type="checkbox"
            checked={Boolean(value)}
            disabled={busy || settled}
            onChange={(event) => setEdit(entry, event.target.checked)}
            data-testid={testId}
          />
          <span>{value ? "on" : "off"}</span>
          {settled && (
            <span className="gated" data-testid={`oneway-${entry.key}`}>
              , one way from here
            </span>
          )}
        </label>
      );
    }

    if (entry.type === "enum") {
      return (
        <select
          className={dirty ? "dirty" : undefined}
          value={String(value ?? "")}
          disabled={busy}
          onChange={(event) => setEdit(entry, Number(event.target.value))}
          data-testid={testId}
        >
          {entry.constraints?.values?.map((option) => (
            <option key={String(option)} value={String(option)}>
              {option === 0 ? "off" : `${option} ${entry.constraints?.unit ?? ""}`.trim()}
            </option>
          ))}
        </select>
      );
    }

    if (entry.type === "integer") {
      return (
        <input
          className={dirty ? "dirty" : undefined}
          type="number"
          min={entry.constraints?.min}
          max={entry.constraints?.max}
          value={String(value ?? "")}
          disabled={busy}
          onChange={(event) => setEdit(entry, Number(event.target.value))}
          style={{ width: 90, textAlign: "right" }}
          data-testid={testId}
        />
      );
    }

    return <span className="num">{formatValue(entry, stored)}</span>;
  }

  function valueFor(entry: SettingEntry): string {
    if (entry.key === "network.lan_address") {
      return data?.deviceAddress ?? "not set";
    }
    if (entry.key === "dashboard.pairing") {
      if (!data?.status) return "unknown";
      return data.status.provisioned ? "paired" : "not paired";
    }
    if (entry.key === "voice.hub") {
      const url = config?.config?.voice.hub_url;
      const tokenSet = config?.config?.voice.hub_token_set;
      if (url === undefined) return "write only";
      if (!url) return "not configured";
      return tokenSet ? `${url}, token set` : `${url}, no token`;
    }
    if (entry.type === "action") {
      return isLive(entry, device) ? "available" : "no route on this firmware";
    }
    if (entry.field) {
      return formatValue(entry, readField(config?.config, entry.field));
    }
    if (entry.key === "network.wifi") return "physical setup only";
    return "unknown remotely";
  }

  const configSupported = config?.supported === true;
  const reading = data ? toDeviceReading(data) : null;

  /** Restart and deep sleep, rendered inside the power panel's danger zone. */
  const dangerActions = SETTINGS_REGISTRY.filter(
    (entry) => entry.type === "action" && isLive(entry, device),
  ).map((entry) => (
    <span key={entry.key} className="danger-action">
      <Button
        variant="danger"
        disabled={busy}
        onClick={() =>
          setActionDialog(entry.key === "system.restart" ? "restart" : "sleep")
        }
        testId={`action-${entry.key}`}
      >
        {entry.label}
      </Button>
      <Disclosure text={entry.note ?? entry.evidence} label={`About ${entry.label}`} />
    </span>
  ));

  /*
   * The skeleton, and how short it now is.
   *
   * This gate is unchanged and still correct — there is genuinely nothing to
   * draw before the first payload — but what it waits for is not what it used
   * to be. It waited on `Promise.all` of two device round trips behind a third
   * from the shell, all three serialised through one mutex, which against an
   * unanswering panel was thirty seconds of this. It now waits on
   * `?observe=0`: local files, no socket, under a millisecond. Everything the
   * page draws from there is marked as the tower's record rather than as a
   * reading, and the real read replaces it when it lands.
   */
  if (!data && !error) {
    return (
      <>
        <div className="page-head">
          <h1>Device</h1>
        </div>
        <PageLoading label="Reading the device." />
      </>
    );
  }

  const battery = data?.power?.battery ?? null;
  const batteryLine =
    battery && battery.percent !== null
      ? `battery ${battery.percent}%${data?.reachable ? " · just measured" : " · at last contact"}`
      : "battery not reported";

  return (
    <>
      <div className="page-head">
        <h1>Device</h1>
        <p>
          Read <FreshnessStamp iso={data?.readAt} />.
        </p>
      </div>

      <ErrorNote>{error}</ErrorNote>

      {notice && (
        <Banner tone="info" testId="apply-notice">
          {notice}
        </Banner>
      )}

      {/*
        Two columns, and the split is by question rather than by subsystem.
        The left is "what is it doing and how much of the time is it awake" —
        the things a person changes. The right is "what is it connected to and
        what else exists" — the things a person checks once and then leaves
        alone, including the two corners of this product that are not part of
        the daily loop at all.
      */}
      <div className="grid-2">
        <section className="stack">
          {reading && (
            <StateStrip
              input={reading.input}
              address={reading.address}
              nextWakeLabel={reading.nextWakeLabel}
              batteryLine={batteryLine}
              onReadAgain={() => void load(true)}
              checking={checking}
              onInteractive={(minutes) =>
                void sendPower(
                  { action: "set-mode", mode: "interactive", minutes },
                  `Interactive for ${minutes} minutes`,
                )
              }
              busy={busy}
            />
          )}

          {/*
            What the last read actually established, and nothing more.

            The badge is chosen from the failure rather than hard-coded, because
            the two cases underneath it are not the same fact and were being
            drawn identically. A read this machine refused out of its own
            routing table — `heldLocally`, and the reason this page is being
            corrected — never reached the network: it does not show that the
            panel is unreachable, only that the tower could not ask. Calling
            that UNREACHABLE beside a badge reading "Asleep" is how one failed
            read came to be rendered as two contradictory claims about a device
            that was, at that moment, answering other clients on the same LAN.
          */}
          {data && data.observed !== false && !data.reachable && (
            <Banner tone="attention" testId="device-unreachable">
              <Badge kind={data.failure?.heldLocally ? "uncertain" : "unreachable"} />
              <span data-testid="device-unreachable-detail">
                {data.detail ?? "The device could not be reached"}
              </span>
            </Banner>
          )}

          {reading && (
            <PowerPanel
              reachable={data?.reachable ?? false}
              observed={data?.observed ?? true}
              power={data?.power ?? null}
              powerSupported={data?.powerSupported ?? null}
              intent={data?.powerIntent ?? null}
              intentDetail={data?.powerIntentDetail ?? ""}
              batteryUnavailableReason={data?.batteryUnavailableReason ?? null}
              reachabilityNote={
                data?.reachabilityNote ?? "The device has not been read yet."
              }
              nextWake={data?.nextWake ?? null}
              deviceLastSeenAt={data?.deviceLastSeenAt ?? null}
              busy={busy}
              dangerActions={dangerActions}
              onSetMode={(mode: PowerMode, options) =>
                sendPower(
                  {
                    action: "set-mode",
                    mode,
                    ...(options?.wakeIntervalMinutes
                      ? { wakeIntervalMinutes: options.wakeIntervalMinutes }
                      : {}),
                  },
                  MODE_COPY[mode].label,
                )
              }
              onCancelIntent={() =>
                sendPower({ action: "cancel-intent" }, "Pending request withdrawn")
              }
              onReconcile={() =>
                sendPower({ action: "reconcile" }, "Delivery of the pending request")
              }
            />
          )}
        </section>

        <section className="stack">
          <RealDeviceCard />

          {/*
            Voice, described honestly and linked rather than hidden.
            Dashed and on the canvas, which is this product's way of saying
            "provisional": the feature exists, configures nothing by default,
            and has never been validated on hardware. That is exactly why it is
            not one of the four navigation entries.
          */}
          <Card
            title="Voice"
            variant="quiet"
            meta={<span className="badge badge-neutral">experimental</span>}
            testId="voice-card"
          >
            <p style={{ color: "var(--ink-soft)", lineHeight: 1.6, margin: 0 }}>
              Push-to-talk on the device can send a voice request to a hub you
              configure. Nothing is configured by default, the microphone stays
              muted, and this has not been validated on hardware.
            </p>
            <div className="card-actions" style={{ marginTop: "var(--pad-2)" }}>
              <Link className="btn" href="/voice">
                Open voice settings →
              </Link>
            </div>
          </Card>

          <Link className="btn card-quiet diag-link" href="/diagnostics">
            Advanced diagnostics — events, versions, send history →
          </Link>
        </section>
      </div>

      {configSupported ? (
        <Banner tone="info" testId="config-supported">
          This firmware reports api {device.api} with {device.capabilities.length}{" "}
          capabilities, including a typed configuration API. Editable rows below are
          live; every other row says exactly why it is not.
        </Banner>
      ) : config === null ? (
        /*
          Neither supported nor unsupported: not read.

          `device.api` falls back to 1 when there is no status, and the
          unsupported banner names that number — so before the config read
          lands this said "Firmware api 1 exposes no configuration API" about a
          device whose firmware nobody had asked. The number was invented by
          the fallback, not reported by the panel.
        */
        <Banner tone="info" testId="config-not-read">
          The tower is reading the device&rsquo;s configuration. Until it
          answers there is no value to show for the settings below, and the
          rows say so rather than guessing at one.
        </Banner>
      ) : (
        <Banner tone="info" testId="config-unsupported">
          Firmware api {device.api} exposes no configuration API, so every setting
          stays on-device only. Rows below say so rather than pretending otherwise.
        </Banner>
      )}

      {conflict && (
        <Banner tone="attention" testId="revision-conflict">
          <Badge kind="neutral">conflict</Badge>
          <span>
            {conflict} The device is now at revision {config?.revision ?? "unknown"}.
            Your edits are still below, next to what the device currently says.
            Review them and apply again, or read the device again to discard them.
          </span>
        </Banner>
      )}

      {Object.keys(applied).length > 0 && (
        <Card title="How the last write took effect">
          {Object.entries(applied).map(([field, mode]) => {
            const entry = SETTINGS_REGISTRY.find((row) => row.field === field);
            return (
              <Row key={field} label={entry?.label ?? field}>
                <Badge kind="neutral">{APPLY_MODE_BADGE[mode]}</Badge>{" "}
                <span>{APPLY_MODE_COPY[mode]}</span>
              </Row>
            );
          })}
        </Card>
      )}

      {configSupported && (
        <Card title="Configuration">
          <Row
            label="Config revision"
            hint="Sent back with every write. The device refuses the write if anything changed since this number was read, including a change somebody made by pressing buttons on it."
          >
            <span className="num" data-testid="config-revision">
              {config?.revision ?? "unknown"}
            </span>
          </Row>
          <Row
            label="Pending changes"
            hint="Edits made here that the device has not been told about yet. Applying sends all of them in one request."
          >
            <span className="num" data-testid="pending-count">
              {pending.length}
            </span>
          </Row>
        </Card>
      )}

      {SECTIONS.map((section) => {
        const entries = SETTINGS_REGISTRY.filter(
          (entry) =>
            entry.section === section &&
            // The two hardware actions moved into the power panel's danger
            // zone, where the other consequential controls live.
            !(entry.type === "action" && isLive(entry, device)),
        );
        if (entries.length === 0) return null;
        return (
          <Card title={section} key={section}>
            {entries.map((entry) => {
              const editable = isEditable(entry, device) && entry.field !== undefined;
              const gated = isGated(entry, device);
              // The hint carries the evidence and the note together: the same
              // words that used to live in a `title` and a tooltip, now in one
              // place a finger can open.
              const hint = gated
                ? gatedTooltip(entry)
                : [entry.evidence, entry.note].filter(Boolean).join(" ");
              return (
                <Row key={entry.key} label={entry.label} hint={hint}>
                  {editable && configSupported ? (
                    <>
                      {renderControl(entry)}{" "}
                      {entry.applyMode && (
                        <Badge kind="neutral" title={APPLY_MODE_COPY[entry.applyMode]}>
                          {APPLY_MODE_BADGE[entry.applyMode]}
                        </Badge>
                      )}
                    </>
                  ) : (
                    <>
                      <span
                        className={gated ? "gated" : ""}
                        data-testid={`setting-${entry.key}`}
                        data-gated={gated ? "true" : "false"}
                      >
                        {valueFor(entry)}
                      </span>{" "}
                      {gated ? (
                        <Badge kind="neutral">on-device only</Badge>
                      ) : (
                        <Badge kind="neutral">read only</Badge>
                      )}
                    </>
                  )}
                </Row>
              );
            })}
          </Card>
        );
      })}

      <AboutAndStorage data={data} narrow={narrow} />

      {/*
        The bar that follows the work. It appears the moment an edit exists,
        carries the count, and holds the typed confirmation when one of the
        edited settings demands one.
      */}
      <StickyActions
        visible={configSupported && pending.length > 0}
        summary={
          <>
            <strong>
              {pending.length} change{pending.length === 1 ? "" : "s"}
            </strong>{" "}
            waiting, at revision {config?.revision ?? "unknown"}
          </>
        }
        detail={
          confirmEntry ? (
            <label className="field">
              <span>
                {confirmEntry.label} needs a typed confirmation. Type{" "}
                <code>{confirmEntry.confirmGate}</code>
              </span>
              <input
                value={confirmWord}
                onChange={(event) => setConfirmWord(event.target.value)}
                autoComplete="off"
                data-testid="batch-confirm"
              />
              <span className="field-hint">{confirmEntry.note}</span>
            </label>
          ) : (
            <Disclosure
              label="What applying costs"
              text="One Apply writes every edited field in a single request. Each write costs a round trip on a device that serves four sockets and runs its own screen through one of them."
            />
          )
        }
      >
        <Button
          variant="quiet"
          disabled={busy}
          onClick={() => {
            setEdits({});
            setConfirmWord("");
          }}
          testId="discard-config"
        >
          Discard
        </Button>
        <Button
          variant="primary"
          disabled={
            busy ||
            pending.length === 0 ||
            (confirmEntry !== undefined && confirmWord !== confirmEntry.confirmGate)
          }
          onClick={() => void apply()}
          testId="apply-config"
        >
          Apply {pending.length} change{pending.length === 1 ? "" : "s"}
        </Button>
      </StickyActions>

      <ConfirmDialog
        open={actionDialog === "restart"}
        title="Restart the device"
        confirmWord="RESTART"
        confirmLabel="Restart it"
        body={
          <>
            <p>
              The device answers first and reboots a second later. Nothing stored
              is lost, and the panel keeps the image it was last given, because
              e-paper holds its image with the power off.
            </p>
            <p>
              It is off the network for as long as it takes to boot and rejoin
              Wi-Fi. The tower cannot tell the difference between that and a
              device that failed to come back until it answers again.
            </p>
          </>
        }
        onConfirm={() => void runAction("restart")}
        onCancel={() => setActionDialog("none")}
      />

      <ConfirmDialog
        open={actionDialog === "sleep"}
        title="Send the device to deep sleep"
        confirmWord="SLEEP"
        confirmLabel="Put it to sleep"
        body={
          <>
            <p>
              The device stops the LAN service and Wi-Fi, then sleeps. From that
              point the tower cannot reach it and cannot wake it.
            </p>
            <p>
              Only the physical BOOT button brings it back. If the device is not
              somewhere you can reach, do not do this.
            </p>
          </>
        }
        onConfirm={() => void runAction("sleep")}
        onCancel={() => setActionDialog("none")}
      />
    </>
  );
}

/**
 * Evidence about the device, rather than controls for it.
 *
 * Folded away on a phone because it is the bottom of a long page and nobody
 * scrolls there to act — they scroll there to check a number when something
 * has already gone wrong. On a desktop there is room, so it stays open.
 */
function AboutAndStorage({
  data,
  narrow,
}: {
  data: StatusPayload | null;
  narrow: boolean;
}) {
  const about = (
    <>
      {data?.status ? (
        aboutFields(data.status).map((field) => (
          <Row key={field.key} label={field.label} hint={field.tooltip}>
            <span className="num">{field.value}</span>
          </Row>
        ))
      ) : data?.observed === false ? (
        <Empty>
          The tower has not read the device yet, so it has nothing to report
          about it. This fills in when the read lands.
        </Empty>
      ) : (
        <Empty>
          The device has not answered, so there is nothing to report about it.
        </Empty>
      )}
      {data?.simulated && (
        <p className="field-hint">
          These values come from the mock device, which reproduces the real
          firmware&apos;s contract. They are not a reading of hardware.
        </p>
      )}
    </>
  );

  const storage = data?.status?.storage ? (
    Object.entries(data.status.storage).map(([key, value]) => (
      <Row
        key={key}
        label={key.replace(/_/g, " ")}
        hint={
          key.includes("failures")
            ? "Non-zero means the filesystem is refusing or losing writes. The panel keeps repainting what it already had, so the symptom is a dashboard that stops updating rather than an obvious error."
            : undefined
        }
      >
        <span className="num">{value.toLocaleString("en-CA")}</span>
      </Row>
    ))
  ) : (
    <Empty>
      {data?.observed === false
        ? "The tower has not read the device yet, so it has no storage counters to show."
        : "No storage counters were reported."}
    </Empty>
  );

  if (narrow) {
    return (
      <Card title="Evidence">
        <Fold summary="About this device" testId="fold-about">
          {about}
        </Fold>
        <Fold summary="Storage counters" testId="fold-storage">
          {storage}
        </Fold>
      </Card>
    );
  }

  return (
    <>
      <Card title="About">{about}</Card>
      <Card title="Storage">{storage}</Card>
    </>
  );
}
