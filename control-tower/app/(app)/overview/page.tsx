"use client";

import { useEffect, useState } from "react";
import Link from "next/link";
import {
  DEVICE_ACTION_COPY,
  INTERACTIVE_REQUEST_NOTE,
  NO_REMOTE_WAKE_NOTE,
  batteryUnavailableReason,
  deriveDeviceState,
  describeWhatHappensNext,
  type DevicePower,
} from "@/core/power";
import { describeQueuedPush } from "@/core/pushQueue";
import { ApiError, apiGet, apiSend, relativeTime, usePolledResource } from "@/ui/api";
import {
  Badge,
  Banner,
  Button,
  Card,
  Cta,
  Empty,
  ErrorNote,
  FreshnessStamp,
  MonoLabel,
  TechDetails,
} from "@/ui/components";
import { Disclosure } from "@/ui/Disclosure";
import { PageError, PageLoading } from "@/ui/PageState";
import { useRegisterPageDeviceState } from "@/ui/pageDeviceState";
import { ScaledPanel } from "@/ui/ScaledPanel";
import { SendFlowDialog } from "@/ui/SendFlowDialog";
import { TemplatePicker } from "@/ui/TemplatePicker";
import { useToast } from "@/ui/Toast";
import { publishDeviceStatus, toDeviceReading } from "@/ui/useDeviceState";

interface SourceRow {
  key: string;
  label: string;
  state: "ok" | "stale" | "unavailable" | "not_configured";
  observedAt: string | null;
  detail: string | null;
}

interface PushRecord {
  pushId: string;
  state: string;
  first: { at: string; dashboardTitle: string; version: number; sha256: string };
  latest: {
    at: string;
    detail: string | null;
    seq: number | null;
    panelMs: number | null;
    /** Wire attempts that did not land. Zero for a push nobody has tried yet. */
    attempts?: number;
  };
}

interface OverviewPayload {
  readAt: string;
  /**
   * When the device and the sources were actually read. Not the same fact.
   *
   * Null when they have not been read yet — see `observed`. The page renders
   * that as "not read yet" rather than as a timestamp, because a freshness
   * stamp on a reading that does not exist is exactly the claim this product
   * refuses to make.
   */
  observedAt: string | null;
  observationStale: boolean;
  snapshotTtlMs: number;
  simulated: boolean;
  tokenConfigured: boolean;
  deviceMode: "mock" | "real";
  deviceAddress: string | null;
  /**
   * Whether the device and the sources below were read at all.
   *
   * False on the first response after a restart, while the read that will
   * replace them runs behind it. Everything else in this payload is local and
   * is as fresh as `readAt` either way, which is why the page renders from it
   * immediately instead of holding a skeleton until a panel answers.
   */
  observed: boolean;
  /** True when a read was started behind this response. */
  refreshing: boolean;
  device:
    | {
        observed: true;
        reachable: true;
        status: {
          firmware: string;
          api: number;
          provisioned: boolean;
          lockdown: boolean;
          stored: { present: boolean; seq: number; sha256: string };
          displayed: { present: boolean; seq: number; sha256: string };
          refresh: { state: string; pending: boolean };
          timing_ms?: Record<string, number>;
        };
      }
    | { observed: boolean; reachable: false; detail: string };
  displayedMatch: { dashboardTitle: string; version: number } | null;
  storedMatch: { dashboardTitle: string; version: number } | null;
  storedEqualsDisplayed: boolean;
  selected: { id: string; title: string; latestVersion: number } | null;
  autoRefresh: {
    intervalMinutes: number | null;
    lastAt: string | null;
    lastDashboardId: string | null;
    lastOutcome: string | null;
  };
  pending: { sha256: string; wouldDedup: boolean } | null;
  sources: SourceRow[];
  composer:
    | { state: "running"; status: Record<string, unknown>; observedAt: string }
    | { state: "not_running" | "unreadable" | "not_configured"; detail: string };
  /** What the device last said, and when. See src/core/power.ts. */
  lastConfirmed: {
    at: string;
    power: DevicePower | null;
    powerSupported: boolean | null;
  } | null;
  blocking: PushRecord | null;
  lastPush: PushRecord | null;
  dashboardCount: number;
  /** The hybrid power block, so the state words agree across pages. */
  power?: DevicePower | null;
  powerSupported?: boolean | null;
  powerIntent?: unknown;
  nextWake?: { at: string; estimated: boolean } | null;
  deviceLastSeenAt?: string | null;
}

interface StatePayload {
  firstRunCompletedAt: string | null;
}

const SOURCE_WORD: Record<SourceRow["state"], string> = {
  ok: "ok",
  stale: "stale",
  unavailable: "unavailable",
  not_configured: "not configured",
};

/**
 * How often the tower is re-read while this tab is in front.
 *
 * Sixty seconds, and it re-reads the *tower* only. The poll is sent without
 * `force=1`, which on `/api/overview` means it is answered from a snapshot
 * the server already holds: no socket at the panel, no forecast fetch, no
 * Home Assistant call, no `remindctl` process. That distinction is the whole
 * reason the snapshot exists — this used to do all four, once a minute, for
 * as long as a tab was open.
 *
 * When the snapshot is older than its own TTL the server answers from it
 * anyway and refreshes behind the response, so a poll never waits on a device
 * either. What the page owes the reader in exchange is the honest age of the
 * reading, which is why `observedAt` is rendered next to the panel and is not
 * the same field as `readAt`.
 */
const POLL_MS = 60_000;

export default function OverviewPage() {
  const overview = usePolledResource<OverviewPayload>("/api/overview", POLL_MS, {
    // While the server is still reading the device and the sources behind its
    // own response, come back in a second rather than at the next minute.
    settling: (payload) => !payload.observed,
    settleMs: 1_000,
  });
  const data = overview.data;
  const toast = useToast();

  const [busy, setBusy] = useState(false);
  const [sendOpen, setSendOpen] = useState(false);
  const [templatesOpen, setTemplatesOpen] = useState(false);
  const [actionError, setActionError] = useState("");
  const [frameStamp, setFrameStamp] = useState(() => Date.now());
  const [firstRun, setFirstRun] = useState<boolean | null>(null);

  // This page carries the device state itself, so the shell knows not to
  // duplicate anything that would stack with it. See src/ui/pageDeviceState.ts.
  useRegisterPageDeviceState();

  // Share the device reading with the shell, so the chip in the header costs
  // no second device read. See src/ui/useDeviceState.ts.
  useEffect(() => {
    if (!data) return;
    publishDeviceStatus({
      reachable: data.device.reachable,
      observed: data.device.observed,
      simulated: data.simulated,
      deviceMode: data.deviceMode,
      deviceAddress: data.deviceAddress,
      power: data.power,
      powerSupported: data.powerSupported ?? null,
      powerIntent: data.powerIntent,
      nextWake: data.nextWake ?? null,
      deviceLastSeenAt: data.deviceLastSeenAt ?? null,
      lastConfirmed: data.lastConfirmed ?? null,
      // The device's own observation time, not this response's. Overview
      // serves a snapshot that can be minutes old, and publishing it under
      // `readAt` would let a cached reading outrank a fresh one the Device
      // page had just taken. See publishDeviceStatus.
      //
      // Undefined when nothing has been observed, because there is no
      // observation time to give. `observed: false` above is what stops that
      // being read as "current" — see publishDeviceStatus.
      readAt: data.observedAt ?? undefined,
    });
  }, [data]);

  useEffect(() => {
    void (async () => {
      try {
        const state = await apiGet<StatePayload>("/api/state");
        setFirstRun(state.firstRunCompletedAt === null);
      } catch {
        // The welcome card is a nicety. A tower that cannot answer has more
        // pressing things to tell the user, and the page says those elsewhere.
        setFirstRun(false);
      }
    })();
  }, []);

  async function dismissFirstRun(): Promise<void> {
    setFirstRun(false);
    try {
      await apiSend("/api/state", "PATCH", { firstRunCompleted: true });
    } catch {
      // Dismissed locally either way: nagging about a failed dismissal is
      // worse than showing the card again next session.
    }
  }

  async function resolveBlocking(
    action: "recheck" | "acknowledge" | "deliver" | "cancel",
  ): Promise<void> {
    if (!data?.blocking) return;
    setBusy(true);
    setActionError("");
    try {
      const response = await apiSend<{ state: string; detail: string }>(
        `/api/device/push/${data.blocking.pushId}`,
        "POST",
        { action },
      );
      toast(
        action === "cancel"
          ? "Queued send withdrawn — nothing was sent"
          : action === "acknowledge"
            ? "Recorded: you accepted an unknown outcome"
            : `${response.state.replace(/_/g, " ")} — ${response.detail}`,
      );
    } catch (caught) {
      setActionError(
        caught instanceof ApiError ? caught.message : "The tower did not answer",
      );
    } finally {
      setBusy(false);
      setFrameStamp(Date.now());
      await overview.reload();
    }
  }

  async function requestInteractive(): Promise<void> {
    setBusy(true);
    try {
      const result = await apiSend<{ applied?: boolean; pending?: boolean; detail?: string }>(
        "/api/device/power",
        "POST",
        { action: "set-mode", mode: "interactive", minutes: 15 },
      );
      toast(
        result.applied
          ? "Interactive for 15 minutes: applied to the device."
          : result.pending
            ? "Interactive for 15 minutes: held until the device next wakes."
            : (result.detail ?? "Request recorded."),
      );
      await overview.reload();
    } catch (caught) {
      setActionError(
        caught instanceof ApiError ? caught.message : "The tower did not answer",
      );
    } finally {
      setBusy(false);
    }
  }

  if (!data) {
    return (
      <>
        <div className="page-head">
          <h1>Overview</h1>
        </div>
        {overview.error ? (
          <PageError message={overview.error} onRetry={() => void overview.reload()} />
        ) : (
          <PageLoading label="Reading the device and the sources." />
        )}
      </>
    );
  }

  const reachable = data.device.reachable;
  const status = reachable ? data.device.status : null;
  const reading = toDeviceReading({
    reachable,
    observed: data.device.observed,
    simulated: data.simulated,
    deviceMode: data.deviceMode,
    deviceAddress: data.deviceAddress,
    power: data.power,
    powerSupported: data.powerSupported ?? null,
    powerIntent: data.powerIntent,
    nextWake: data.nextWake ?? null,
    deviceLastSeenAt: data.deviceLastSeenAt ?? null,
    lastConfirmed: data.lastConfirmed ?? null,
    readAt: data.observedAt ?? undefined,
  });
  const state = deriveDeviceState(reading.input);

  const queued = data.blocking?.state === "queued" ? data.blocking : null;
  /*
   * An unresolved send, which is not the same thing as an unreachable device.
   *
   * `pending`, `sent` and `uncertain` all mean the tower put a frame on the
   * wire and cannot say what became of it. Until a human resolves one of them
   * no new push may go out, and the picture above is therefore the last frame
   * the device *confirmed* rather than the current one — which is why the
   * label over the panel changes when this is set.
   */
  const unresolved =
    data.blocking && data.blocking.state !== "queued" ? data.blocking : null;

  const battery = data.power?.battery ?? null;
  const batteryLine =
    battery && battery.percent !== null
      ? `battery ${battery.percent}%${reachable ? " · just measured" : " · at last contact"}`
      : "battery not reported";

  /*
   * What the picture below is, said in the heading.
   *
   * "CONFIRMED" is a claim about a reading, so it is only made when there has
   * been one. On the first paint the device has not been asked and the heading
   * says only what the block is about.
   */
  const panelLabel = !data.device.observed
    ? "ON THE PANEL"
    : unresolved
      ? "LAST CONFIRMED IMAGE"
      : "ON THE PANEL — CONFIRMED";
  const hasFrame = reachable && status?.displayed.present === true;

  return (
    <>
      <h1 className="visually-hidden">Overview</h1>

      {overview.error && <ErrorNote>{overview.error}</ErrorNote>}
      {actionError && <ErrorNote>{actionError}</ErrorNote>}

      {/*
        The honest part of polling: when the tower itself has stopped answering
        — a laptop that slept, a server that died — the page says so instead of
        letting the numbers below age silently. This is about the POLL failing,
        which is a different fact from the snapshot being stale: that one is
        reported next to the panel, because the tower is answering perfectly
        well and simply has not re-read the device yet.
      */}
      {overview.stale && (
        <Banner tone="pending" testId="stale-banner">
          <Badge kind="pending" />
          <span>
            The tower has not answered for more than{" "}
            {Math.round((POLL_MS * 2) / 60_000)} minutes, so everything below is
            at least that old and may have moved on.
          </span>
          <Button onClick={() => void overview.reload()} disabled={busy} testId="stale-retry">
            Read again
          </Button>
        </Banner>
      )}

      {firstRun && (
        // Three lines, not three paragraphs. This card sits directly above
        // the panel — the picture the product is about — and the long version
        // pushed that off the first screenful on a phone, so a new user's
        // introduction to the tower was advice about something they could not
        // see. Each step now says what to do and links to the page that
        // explains it in place.
        <Card title="Welcome to the control tower" testId="onboarding">
          <div className="onboarding">
            <ol>
              <li>
                <strong>Nothing here is hardware.</strong>
                Every SIMULATED reading is the in-repo mock;{" "}
                <Link href="/diagnostics">Advanced</Link> shows what the tower is
                talking to.
              </li>
              <li>
                <strong>Make your first composition.</strong>
                <Link href="/dashboards">Compositions</Link> starts you with a
                saved layout to show and edit.
              </li>
              <li>
                <strong>Then the real panel, if you have one.</strong>
                <Link href="/device">Device</Link> takes two confirmations, and
                every send costs a full refresh cycle.
              </li>
            </ol>
            <Button onClick={() => void dismissFirstRun()} testId="dismiss-onboarding">
              Do not show this again
            </Button>
          </div>
        </Card>
      )}

      <div className="grid-2">
        {/* ------------------------------------------------- the panel -- */}
        <section style={{ minWidth: 0 }}>
          <div
            style={{
              display: "flex",
              alignItems: "baseline",
              justifyContent: "space-between",
              gap: "var(--pad-2)",
              flexWrap: "wrap",
              marginBottom: 10,
            }}
          >
            <h2 data-testid="panel-label">{panelLabel}</h2>
            <span className="mono-meta" data-testid="panel-provenance">
              {/*
                "device-reported", not "sent". The distinction is the whole
                claim this line makes: the picture above is what the device
                said it is displaying, read at this time, rather than what the
                tower last tried to put there.

                And only when the device actually answered. This line sat above
                a card whose body was a transport error and still read
                "device-reported · just now", which claims a reading that never
                happened — the same conflation of "we could not ask" with "the
                device told us" that the badge underneath it was making.
              */}
              {!data.device.observed ? (
                // Not a failed read, so not "no answer": nothing has been
                // asked. The read that will answer it is already running
                // behind the response this line came from.
                <span data-testid="panel-not-read">reading the device…</span>
              ) : (
                <>
                  {reachable ? "device-reported" : "no answer · last tried"} ·{" "}
                  <FreshnessStamp iso={data.observedAt} />
                  {data.observationStale && (
                    <span className="gated" data-testid="observation-stale">
                      {" "}
                      · refreshing
                    </span>
                  )}
                </>
              )}
            </span>
          </div>

          <div className="card card-lg" data-testid="on-panel">
            <div className="card-body">
              {hasFrame ? (
                <>
                  <ScaledPanel inset={36} max={1.45}>
                    {/* eslint-disable-next-line @next/next/no-img-element */}
                    <img
                      src={`/api/device/frame?t=${frameStamp}`}
                      alt="The frame the device reports it is displaying"
                      width={400}
                      height={300}
                      style={{
                        width: 400,
                        height: 300,
                        display: "block",
                        imageRendering: "pixelated",
                      }}
                      data-testid="panel-frame"
                    />
                  </ScaledPanel>

                  <div
                    style={{
                      display: "flex",
                      justifyContent: "space-between",
                      alignItems: "center",
                      gap: 10,
                      flexWrap: "wrap",
                      marginTop: 12,
                    }}
                  >
                    <strong style={{ fontSize: "var(--size-14)" }}>
                      {data.displayedMatch ? (
                        <>
                          {data.displayedMatch.dashboardTitle} v
                          {data.displayedMatch.version}
                        </>
                      ) : (
                        <span data-testid="unknown-frame">
                          An unknown frame — not sent from this tower
                        </span>
                      )}
                    </strong>
                    <span className="mono-meta">400 × 300 · 4 colours</span>
                  </div>

                  <TechDetails testId="panel-tech">
                    frame digest {status.displayed.sha256.slice(0, 8)}…
                    {status.displayed.sha256.slice(-4)} · device sequence{" "}
                    {status.displayed.seq}
                    {"\n"}
                    {data.storedEqualsDisplayed
                      ? "stored and displayed are the same frame"
                      : "a newer frame is stored and the panel has not refreshed to it yet"}
                    {"\n"}
                    reported by the device itself, not inferred from the send
                  </TechDetails>
                </>
              ) : !data.device.observed ? (
                /*
                  "Nothing is stored on the device" is a fact the device
                  reports, and it has not been asked. Saying it here would be
                  telling a reader their panel is blank on the strength of a
                  read that had not happened — and it would be wrong for the
                  overwhelmingly common case, a panel holding the image it was
                  sent an hour ago.
                */
                <Empty>
                  <span data-testid="panel-unread">
                    The tower has not read the device yet, so it cannot say what
                    the panel is showing. The panel itself is unaffected: it
                    holds its last image whether or not anyone is looking.
                  </span>
                </Empty>
              ) : (
                <Empty>
                  Nothing is stored on the device yet, so there is nothing to
                  show.{" "}
                  {!reachable && data.device.reachable === false
                    ? data.device.detail
                    : ""}
                </Empty>
              )}
            </div>
          </div>

          {unresolved && (
            <p
              style={{
                margin: "10px 2px 0",
                fontWeight: 500,
                fontSize: "var(--size-13)",
                color: "var(--accent-red)",
              }}
              data-testid="uncertain-note"
            >
              A newer send is unconfirmed — this is the last image the device
              confirmed.
            </p>
          )}

          {/* ------------------------------------------- the quiet facts --
              Under the picture, because they are all facts *about* it: which
              sources fed it, and what happened the last time one was sent.
              None of this is what somebody opened the page for, and none of it
              was deleted to make room — it is simply below the fold now rather
              than competing with the panel for the top of the screen. */}
          <div className="stack" style={{ marginTop: "var(--pad-4)" }}>
            <Card title="Sources" variant="plain">
              {!data.observed ? (
                /*
                  Empty because nothing has been read, not because nothing is
                  configured. The sentence below it — "select a composition" —
                  is the right answer to an empty list and the wrong answer to
                  an unread one, and a reader who has selected a composition
                  would be told to go and do what they had just done.
                */
                <Empty>
                  <span data-testid="sources-not-read">
                    Reading the sources. This card fills in as they answer.
                  </span>
                </Empty>
              ) : data.sources.length === 0 ? (
                <Empty>
                  Select a composition to see which sources it actually reads.
                  The tower fetches nothing a composition does not use.
                </Empty>
              ) : (
                <div className="listing" style={{ border: "none" }}>
                  {data.sources.map((row) => (
                    <div className="listing-row" key={row.key}>
                      <span className="listing-msg">{row.label}</span>
                      <span className="mono-meta">
                        <span className={row.state === "ok" ? "" : "gated"}>
                          {SOURCE_WORD[row.state]}
                        </span>{" "}
                        {row.observedAt && <FreshnessStamp iso={row.observedAt} />}
                      </span>
                    </div>
                  ))}
                </div>
              )}
              {/* Only when an origin is configured. An installation that never
                  set one has no second composer to report on, and a row saying
                  so forever would be furniture. */}
              {/*
                Hidden until something has actually been read. `unread` reports
                the composer as unreadable because it has not looked, and
                rendering "Existing composer: Not read yet." next to a sentence
                about a program this tower never writes to is noise about a
                thing nobody asked after.
              */}
              {data.observed && data.composer.state !== "not_configured" && (
                <p className="mono-note" style={{ marginTop: "var(--pad-2)" }}>
                  Existing composer:{" "}
                  {data.composer.state === "running"
                    ? "running"
                    : data.composer.detail}
                  . A separate renderer this tower can read but never writes to.
                </p>
              )}
            </Card>

            <Card title="Last send" variant="plain">
              {data.lastPush ? (
                <>
                  <p style={{ margin: 0 }}>
                    <Badge
                      kind={
                        data.lastPush.state === "verified_displayed"
                          ? "displayed"
                          : data.lastPush.state === "failed"
                            ? "failed"
                            : data.lastPush.state === "uncertain"
                              ? "uncertain"
                              : "pending"
                      }
                    >
                      {data.lastPush.state.replace(/_/g, " ")}
                    </Badge>{" "}
                    {data.lastPush.first.dashboardTitle} v
                    {data.lastPush.first.version},{" "}
                    <FreshnessStamp iso={data.lastPush.latest.at} />
                  </p>
                  <p className="mono-note" style={{ marginTop: 6 }}>
                    {data.lastPush.latest.detail ?? "no detail"}
                  </p>
                </>
              ) : (
                <Empty>Nothing has been sent from this tower yet.</Empty>
              )}
              {data.autoRefresh.intervalMinutes !== null && (
                <p className="mono-note" style={{ marginTop: "var(--pad-2)" }}>
                  automatic refresh every {data.autoRefresh.intervalMinutes} min
                  {data.autoRefresh.lastAt && (
                    <>
                      {" "}
                      · last <FreshnessStamp iso={data.autoRefresh.lastAt} />
                      {data.autoRefresh.lastOutcome
                        ? ` · ${data.autoRefresh.lastOutcome}`
                        : ""}
                    </>
                  )}
                </p>
              )}
            </Card>
          </div>
        </section>

        {/* ------------------------------------------------ the device -- */}
        <section className="stack">
          {queued && (
            <div className="pending-card" data-testid="queued-banner">
              <MonoLabel>Queued for next wake</MonoLabel>
              <p data-testid="queued-detail">
                &ldquo;{queued.first.dashboardTitle} v{queued.first.version}
                &rdquo; is rendered and waiting. Nothing has been sent to the
                device.{" "}
                {describeQueuedPush({
                  queued: {
                    queuedAt: queued.first.at,
                    attempts: queued.latest.attempts ?? 0,
                    lastAttemptAt: queued.latest.at,
                    lastError: queued.latest.detail,
                  },
                  reachable,
                  deviceMode: data.deviceMode,
                  nextWakeLabel: reading.nextWakeLabel,
                  noRemoteWakeNote: NO_REMOTE_WAKE_NOTE,
                })}
              </p>
              <div className="card-actions">
                <Button
                  onClick={() => void resolveBlocking("deliver")}
                  disabled={busy}
                  testId="deliver-queued"
                  title="Try to send it now. If the device is still asleep this changes nothing and says so."
                >
                  Try sending now
                </Button>
                <Button
                  onClick={() => void resolveBlocking("cancel")}
                  disabled={busy}
                  testId="cancel-queued"
                >
                  Cancel
                </Button>
              </div>
            </div>
          )}

          {/*
            `data-state` on the wrapper, carrying the same word the header chip
            shows. It is the hook the browser QA asserts against on both pages
            that render the device's state, which is how "one reading, one
            word" stops being a claim in a comment and becomes something a test
            can fail on.
          */}
          <div data-testid="state-strip" data-device-state={state.state} data-state={state.state}>
          <Card
            title="Device"
            variant="plain"
            meta={batteryLine}
            testId="device-narrative"
          >
            <div className="know-grid">
              <div>
                <MonoLabel>What we know</MonoLabel>
                <p data-testid="know">
                  <strong data-testid="state-strip-badge">{state.label}.</strong>{" "}
                  {state.reason}
                </p>
              </div>
              <div>
                <MonoLabel>What you can do now</MonoLabel>
                <p data-testid="can-do">{DEVICE_ACTION_COPY[state.state]}</p>
              </div>
              <div>
                <MonoLabel>What happens next</MonoLabel>
                <p data-testid="happens-next">
                  {describeWhatHappensNext(state.state, reading.nextWakeLabel)}
                </p>
              </div>
            </div>

            {battery && battery.percent === null && (
              <p className="mono-note" style={{ marginTop: "var(--pad-2)" }}>
                {batteryUnavailableReason(data.power as DevicePower)}
              </p>
            )}

            {/*
              The two controls that belong with the state rather than with the
              picture: read the device again, and ask it to stay awake. The
              second carries its caveat in a disclosure rather than a `title`,
              because what it does and does not do — applied now if the device
              is awake, held if it is not, and never a wake — is the product's
              own contract, and a `title` publishes it to a hovering mouse and
              to nobody else. See INTERACTIVE_REQUEST_NOTE in src/core/power.ts.
            */}
            <div className="card-actions" style={{ marginTop: "var(--pad-3)" }}>
              {/*
                Its own button, and nothing else on the page.

                Disabled on `reloading` rather than on `loading`: the second is
                also true for the sixty-second poll and for the short follow-up
                after a provisional answer, and a control that greys itself out
                on a timer nobody triggered reads as a page that has seized up.
                The label says what it is doing, because this read is allowed to
                take real time — up to twenty-one seconds of read-only retries
                when this machine is refusing connections to the panel out of
                its own routing table — and a button that looks inert for
                twenty seconds gets pressed four more times.
              */}
              <Button
                onClick={() => void overview.reload()}
                disabled={busy || overview.reloading}
                testId="refresh"
              >
                {overview.reloading ? "Checking…" : "Check now"}
              </Button>
              <Button
                onClick={() => void requestInteractive()}
                disabled={busy}
                testId="state-strip-interactive"
              >
                Interactive 15 min
              </Button>
              <Disclosure
                text={INTERACTIVE_REQUEST_NOTE}
                label="What asking for interactive does"
                testId="state-strip-interactive-why"
              />
            </div>

            {unresolved && (
              <div className="resolve-box" data-testid="blocking-banner">
                <p>
                  Sending is paused until this is resolved — that&rsquo;s what
                  prevents accidental double refreshes.
                </p>
                <p className="mono-note" style={{ marginBottom: "var(--pad-2)" }}>
                  {unresolved.first.dashboardTitle} v{unresolved.first.version} is{" "}
                  {unresolved.state}. {unresolved.latest.detail}
                </p>
                <div className="resolve-actions">
                  <Button
                    variant="dark"
                    onClick={() => void resolveBlocking("recheck")}
                    disabled={busy}
                    testId="recheck"
                  >
                    Re-check the device
                  </Button>
                  <Button
                    onClick={() => void resolveBlocking("acknowledge")}
                    disabled={busy}
                    testId="acknowledge"
                  >
                    Accept unknown outcome
                  </Button>
                </div>
              </div>
            )}
          </Card>
          </div>

          <div className="stack">
            <Cta
              glyph="→"
              onClick={() => setSendOpen(true)}
              disabled={busy || data.selected === null}
              testId="push-button"
            >
              Show a composition
            </Cta>
            <Cta
              glyph="+"
              variant="default"
              onClick={() => setTemplatesOpen(true)}
              testId="new-composition"
            >
              New composition
            </Cta>
            {data.selected === null && (
              <p className="field-hint" data-testid="no-selection">
                No composition is selected. Choose one on{" "}
                <Link href="/dashboards">Compositions</Link>, or start a new one.
              </p>
            )}
            {data.selected !== null && (
              <p className="field-hint">
                Sends{" "}
                <Link href={`/dashboards/${data.selected.id}/edit`}>
                  {data.selected.title}
                </Link>{" "}
                v{data.selected.latestVersion}
                {data.pending?.wouldDedup && (
                  <span data-testid="would-dedup">
                    {" "}
                    — unchanged since the last confirmed send, so it would be
                    skipped.
                  </span>
                )}
              </p>
            )}
          </div>

        </section>
      </div>

      <SendFlowDialog
        open={sendOpen}
        dashboardId={data.selected?.id ?? null}
        dashboardTitle={data.selected?.title ?? ""}
        device={reading}
        onClose={() => setSendOpen(false)}
        onSettled={() => {
          setFrameStamp(Date.now());
          void overview.reload();
        }}
      />

      <TemplatePicker
        open={templatesOpen}
        onClose={() => setTemplatesOpen(false)}
        onCreated={(title) => toast(`Started from “${title}” — saved as v1`)}
      />

      {/*
        A hidden live line carrying how long ago the device was seen. The
        relative stamps above update on their own timers; this is the one that
        a screen reader is offered when the poll lands, so a reader who cannot
        see the timestamps move is still told the page re-read.
      */}
      <span className="visually-hidden" role="status">
        {reachable
          ? `Device answered ${relativeTime(data.deviceLastSeenAt)}.`
          : "Device did not answer the last read."}
      </span>
    </>
  );
}
