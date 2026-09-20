"use client";

import { useCallback, useEffect, useState } from "react";
import { ApiError, apiGet, absoluteTime } from "@/ui/api";
import {
  Badge,
  Button,
  Card,
  Empty,
  ErrorNote,
  FreshnessStamp,
  Row,
} from "@/ui/components";
import { PageLoading } from "@/ui/PageState";

interface PushRecord {
  pushId: string;
  state: string;
  first: {
    at: string;
    dashboardTitle: string;
    version: number;
    sha256: string;
    semanticHash: string;
    deviceMode: string;
    forced: boolean;
  };
  latest: { at: string; detail: string | null; seq: number | null; panelMs: number | null };
}

interface AuditEntry {
  at: string;
  action: string;
  target: string;
  outcome: string;
  deviceConfirmed: boolean | null;
  detail: string | null;
}

interface DiagnosticsPayload {
  tower: {
    dataRoot: string;
    deviceMode: string;
    simulated: boolean;
    deviceOrigin: string;
    atlases: string[];
    atlasRaster: string;
    fontFamilies: Array<{
      id: string;
      label: string;
      licence: string;
      copyright: string;
      upstream: string;
      vendoredFrom: string;
      licenceFile: string;
    }>;
    pictograms: {
      raster: string;
      count: number;
      categories: Array<{ category: string; symbols: string }>;
    };
    node: string;
    bind: {
      verdict: "allowed" | "refused" | "undetermined";
      source: string;
      hosts: string[];
      refusals: string[];
      summary: string;
    };
    version: string;
  };
  device: {
    reachable: boolean;
    status?: Record<string, unknown>;
    detail?: string;
  };
  pushes: PushRecord[];
  audit: AuditEntry[];
  composer: { state: string; detail?: string; status?: unknown };
  sources: {
    calendar: { state: string; detail: string | null };
    homeAssistant: { configured: boolean };
    reminders: {
      binary: string;
      defaultList: string;
      readOnlySubcommands: string[];
      note: string;
    };
  };
}

type Tab = "events" | "facts" | "history" | "tower";

const TABS: ReadonlyArray<{ id: Tab; label: string }> = [
  { id: "events", label: "Events" },
  { id: "facts", label: "Device facts" },
  { id: "history", label: "Send history" },
  { id: "tower", label: "Tower" },
];

/**
 * A ledger state, as a tag and a colour.
 *
 * Six words, and each one means a different thing about the panel:
 *
 *   displayed     the device said it is showing this. The only certainty.
 *   uncertain     it went out and nothing came back. Needs a human.
 *   skipped       identical to what is already there; no refresh was spent.
 *   failed        the device answered and refused, or could not be reached.
 *   queued        rendered and held. Nothing has been sent.
 *   acknowledged  a human accepted an unknown outcome, on the record.
 *
 * "Skipped" is the one that is not a `PushState`: dedup never reaches the
 * ledger as a line of its own, so a record that carries `deduped` is labelled
 * from that rather than from its state. Everything else is the state verbatim.
 */
function tagFor(record: PushRecord): { text: string; kind: "displayed" | "uncertain" | "failed" | "pending" | "neutral" } {
  switch (record.state) {
    case "verified_displayed":
      return { text: "displayed", kind: "displayed" };
    case "uncertain":
      return { text: "uncertain", kind: "uncertain" };
    case "failed":
      return { text: "failed", kind: "failed" };
    case "acknowledged":
      return { text: "acknowledged", kind: "neutral" };
    case "queued":
      return { text: "queued", kind: "pending" };
    default:
      return { text: record.state.replace(/_/g, " "), kind: "pending" };
  }
}

/** The colour of the dot beside an audit line, from its outcome. */
function dotClass(outcome: string): string {
  if (outcome === "ok") return "dot-awake";
  if (outcome === "refused" || outcome === "failed") return "dot-unreachable";
  return "dot-asleep";
}

export default function DiagnosticsPage() {
  const [data, setData] = useState<DiagnosticsPayload | null>(null);
  const [error, setError] = useState("");
  const [tab, setTab] = useState<Tab>("events");

  const load = useCallback(async () => {
    try {
      setData(await apiGet<DiagnosticsPayload>("/api/diagnostics"));
      setError("");
    } catch (caught) {
      setError(caught instanceof ApiError ? caught.message : "The tower did not answer");
    }
  }, []);

  useEffect(() => {
    void load();
  }, [load]);

  /**
   * The device's own facts, as key/value rows.
   *
   * Built from whatever the status actually carried, with "not reported"
   * spelled out for anything it did not. That phrase is load-bearing: a device
   * that does not report its board revision is a different fact from a tower
   * that forgot to ask, and a blank row conflates them.
   */
  const facts: Array<[string, string]> = data
    ? (() => {
        const status = data.device.status ?? {};
        const device = (status.device ?? {}) as Record<string, unknown>;
        const read = (value: unknown): string =>
          value === undefined || value === null || value === ""
            ? "not reported"
            : String(value);
        return [
          ["Device", read(device.name ?? device.model)],
          ["Firmware", read(status.firmware ?? device.fw)],
          ["Panel", "400 × 300, four colours"],
          ["Hardware", read(device.hardware)],
          ["Board", read(device.model)],
          ["Device API", read(status.api)],
          [
            "Capabilities",
            Array.isArray(status.capabilities) && status.capabilities.length > 0
              ? (status.capabilities as string[]).join(", ")
              : "not reported",
          ],
          ["Config revision", read(status.config_revision)],
          ["Provisioned", status.provisioned === undefined ? "not reported" : status.provisioned ? "paired" : "not paired"],
        ];
      })()
    : [];

  return (
    <>
      <div className="page-head">
        <h1>Diagnostics</h1>
        <div className="card-actions">
          <span className="mono-meta">Nothing here is needed day to day.</span>
          {/*
            Beside the title rather than in the tab row. It is an action, and a
            quiet button sitting in a row of tabs reads as a fifth tab you have
            not visited yet.
          */}
          <Button variant="quiet" onClick={() => void load()} testId="refresh">
            Read again
          </Button>
        </div>
      </div>

      <ErrorNote>{error}</ErrorNote>
      {!data && !error && <PageLoading label="Reading the tower." />}

      {data && (
        <>
          <div className="tab-row" role="group" aria-label="Diagnostics sections">
            {TABS.map((entry) => (
              <Button
                key={entry.id}
                ariaPressed={tab === entry.id}
                onClick={() => setTab(entry.id)}
                testId={`diag-tab-${entry.id}`}
              >
                {entry.label}
              </Button>
            ))}
          </div>

          {/* ------------------------------------------------- events -- */}
          {tab === "events" && (
            <div className="listing" data-testid="audit-table">
              {data.audit.length === 0 ? (
                <div className="listing-row">
                  <Empty>No mutating actions recorded yet.</Empty>
                </div>
              ) : (
                data.audit.map((entry, index) => (
                  <div className="listing-row" key={`${entry.at}-${index}`}>
                    <span className="listing-time" title={absoluteTime(entry.at)}>
                      <FreshnessStamp iso={entry.at} />
                    </span>
                    <span
                      className={`listing-dot ${dotClass(entry.outcome)}`}
                      aria-hidden="true"
                    />
                    <span className="listing-msg">
                      <code>{entry.action}</code> {entry.target} — {entry.outcome}
                      {entry.deviceConfirmed !== null && (
                        <>
                          {" "}
                          · device confirmed: {entry.deviceConfirmed ? "yes" : "no"}
                        </>
                      )}
                      {entry.detail ? <> · {entry.detail}</> : null}
                    </span>
                  </div>
                ))
              )}
              <p className="listing-foot">
                Every mutating action, in the order it happened. Secrets never
                appear here: any parameter whose name looks like a credential is
                replaced before the entry is written. This is a log, not an
                archive.
              </p>
            </div>
          )}

          {/* -------------------------------------------- device facts -- */}
          {tab === "facts" && (
            <>
              <div className="listing" data-testid="device-facts">
                {facts.map(([key, value]) => (
                  <div className="listing-row" key={key}>
                    <span className="listing-key">{key}</span>
                    <span className="listing-val">{value}</span>
                  </div>
                ))}
                <p className="listing-foot">
                  Everything above is reported by the device itself.
                  &ldquo;Not reported&rdquo; means exactly that — it is not a
                  value the tower failed to read, it is a value the device did
                  not send.
                </p>
              </div>

              <Card title="Device status, raw" variant="plain">
                {data.device.reachable ? (
                  <pre className="json" data-testid="device-json">
                    {JSON.stringify(data.device.status, null, 2)}
                  </pre>
                ) : (
                  <Empty>{data.device.detail}</Empty>
                )}
              </Card>
            </>
          )}

          {/* ------------------------------------------- send history -- */}
          {tab === "history" && (
            <div className="listing" data-testid="ledger-table">
              {data.pushes.length === 0 ? (
                <div className="listing-row">
                  <Empty>Nothing has been sent from this tower yet.</Empty>
                </div>
              ) : (
                data.pushes.map((record) => {
                  const tag = tagFor(record);
                  return (
                    <div className="listing-row" key={record.pushId}>
                      <span
                        className="listing-time"
                        title={absoluteTime(record.latest.at)}
                      >
                        <FreshnessStamp iso={record.latest.at} />
                      </span>
                      <Badge kind={tag.kind}>{tag.text}</Badge>
                      <span className="listing-msg">
                        {record.first.dashboardTitle} v{record.first.version}
                        {record.first.forced && " (forced)"} ·{" "}
                        <code title={record.first.sha256}>
                          {record.first.sha256.slice(0, 12)}
                        </code>
                        {record.latest.seq !== null && <> · seq {record.latest.seq}</>}
                        {record.latest.panelMs !== null && (
                          <> · refresh {(record.latest.panelMs / 1000).toFixed(1)} s</>
                        )}
                        {record.latest.detail ? <> · {record.latest.detail}</> : null}
                      </span>
                    </div>
                  );
                })
              )}
              <p className="listing-foot">
                Append only: a state change adds a line and a line is never
                rewritten. &ldquo;Displayed&rdquo; means the device said so. A
                send with no confirmation stays &ldquo;uncertain&rdquo; until a
                human resolves it. The digest is also the map from what the
                device reports back to the version that produced it.
              </p>
            </div>
          )}

          {/* -------------------------------------------------- tower -- */}
          {tab === "tower" && (
            <>
              <Card title="Tower" variant="plain">
                <p className="field-hint">
                  The tower as it sees itself. If you are opening an issue, copy
                  this card: it is every fact about the build that a reader needs
                  and contains nothing private.
                </p>
                <Row label="Version">
                  <code data-testid="tower-version">{data.tower.version}</code>
                </Row>
                <Row label="Device mode">
                  {data.tower.simulated ? <Badge kind="simulated" /> : "real"}
                </Row>
                <Row label="Device origin">
                  <code>{data.tower.deviceOrigin}</code>
                </Row>
                <Row label="Data root" hint="Outside the repository, 0700">
                  <code>{data.tower.dataRoot}</code>
                </Row>
                <Row label="Node">
                  <code>{data.tower.node}</code>
                </Row>
                {/*
                  The bind guard, reported rather than promised. "undetermined"
                  is shown as plainly as the other two: a reader who is about to
                  put this on a network needs to know when the in-process check
                  could not see the socket, and a row that said "loopback"
                  regardless would be the exact kind of reassuring lie this page
                  exists to refuse.
                */}
                <Row label="Bind guard" hint={data.tower.bind.summary}>
                  <span
                    className={data.tower.bind.verdict === "allowed" ? "" : "gated"}
                    data-testid="bind-verdict"
                  >
                    {data.tower.bind.verdict}
                  </span>{" "}
                  <span className="field-hint">
                    (observed from: {data.tower.bind.source})
                  </span>
                </Row>
                <Row
                  label="Glyph atlases"
                  hint={`Rasterised ${data.tower.atlasRaster}. Committed to the repository, so a render needs no font on this machine and no asset off it.`}
                >
                  <span className="num">{data.tower.atlases.length}</span>
                </Row>
              </Card>

              {/*
                Where the letters on the panel come from, and under what
                licence. This is the provenance that belongs HERE and not on a
                400x300 e-paper panel, which is the same rule the weather module
                follows.
              */}
              <Card title="Fonts" variant="plain">
                {data.tower.fontFamilies.map((family) => (
                  <Row
                    key={family.id}
                    label={family.label}
                    hint={`${family.copyright}. Vendored from ${family.vendoredFrom}; licence text at ${family.licenceFile}.`}
                  >
                    <span>
                      {family.licence} ·{" "}
                      <code data-testid={`font-upstream-${family.id}`}>
                        {family.upstream}
                      </code>
                    </span>
                  </Row>
                ))}
                <p className="field-hint">
                  Every face is redistributable, which is why its rasterised
                  glyphs can live in this repository. Regenerate them with
                  tools/gen_font_atlas.py; nothing is fetched at runtime.
                </p>
                {/*
                  The rows above are the faces the *panel* can be set in — the
                  ones with a committed glyph atlas behind them. These two are
                  the faces this browser page is set in, which is a different
                  question and was previously not answered anywhere: a reader
                  checking that nothing is fetched from a font CDN had to take
                  it on trust.
                */}
                <hr />
                <Row
                  label="Interface faces"
                  hint="Self-hosted by next/font/local from assets/fonts/, read at build time. No CDN, and tools/screenshots.ts fails its QA run on a request to any origin but this one."
                >
                  <span data-testid="interface-faces">
                    Space Grotesk · IBM Plex Mono, both OFL 1.1
                  </span>
                </Row>
              </Card>

              {/*
                Emoji on a four-colour panel. There is no emoji font here and
                nothing is downloaded: these are sprites this project drew,
                which is the only way to put a symbol on this device without
                redistributing somebody else's colour bitmaps.
              */}
              <Card title="Symbols" variant="plain">
                <Row label="Supported symbols" hint={data.tower.pictograms.raster}>
                  <span className="num" data-testid="pictogram-count">
                    {data.tower.pictograms.count}
                  </span>
                </Row>
                {data.tower.pictograms.categories.map((group) => (
                  <Row key={group.category} label={group.category}>
                    <span data-testid={`pictograms-${group.category}`}>
                      {group.symbols}
                    </span>
                  </Row>
                ))}
                <p className="field-hint">
                  Anything outside this set is drawn as a boxed question mark and
                  named in the designer, so a symbol is never silently dropped
                  and never renders as an empty font box. Apple Color Emoji is
                  not used: it cannot be redistributed and it is not a
                  four-colour image.
                </p>
              </Card>

              {data.composer.state !== "not_configured" && (
                <Card title="Existing composer" variant="plain">
                  <Row label="State">{data.composer.state.replace(/_/g, " ")}</Row>
                  {data.composer.state === "running" ? (
                    <pre className="json">
                      {JSON.stringify(data.composer.status, null, 2)}
                    </pre>
                  ) : (
                    <Empty>{data.composer.detail}</Empty>
                  )}
                  <p className="field-hint">
                    Read only, at the origin in NOTE4C_COMPOSER_ORIGIN. The tower
                    never writes to it, never takes its scheduler lock, and never
                    calls its legacy gallery upload path.
                  </p>
                </Card>
              )}

              <Card title="Sources" variant="plain">
                <Row
                  label="Calendar snapshot"
                  hint={data.sources.calendar.detail ?? undefined}
                >
                  {data.sources.calendar.state}
                </Row>
                <Row label="Home Assistant credentials">
                  {data.sources.homeAssistant.configured ? "present" : "not configured"}
                </Row>
                <Row label="Apple Reminders" hint={data.sources.reminders.note}>
                  <span data-testid="reminders-mode">
                    read only · {data.sources.reminders.readOnlySubcommands.join(", ")}
                  </span>
                </Row>
                <Row label="Reminders list">
                  <code data-testid="reminders-list">
                    {data.sources.reminders.defaultList}
                  </code>
                </Row>
                <p className="field-hint">
                  This page describes the Reminders adapter; it does not run it.
                  The list is read only when a composition binds to it, only the
                  count of open reminders ever leaves the Mac, and a
                  reminder&rsquo;s notes are dropped at the parse boundary rather
                  than carried and ignored.
                </p>
              </Card>
            </>
          )}
        </>
      )}
    </>
  );
}
