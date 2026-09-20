"use client";

import { useCallback, useEffect, useState } from "react";
import Link from "next/link";
import { useRouter } from "next/navigation";
import { ApiError, apiGet, apiSend, relativeTime } from "@/ui/api";
import { Badge, Button, ErrorNote } from "@/ui/components";
import { PageEmpty, PageLoading } from "@/ui/PageState";
import { SendFlowDialog } from "@/ui/SendFlowDialog";
import { TemplatePicker } from "@/ui/TemplatePicker";
import { useToast } from "@/ui/Toast";
import { publishDeviceStatus, useDeviceState } from "@/ui/useDeviceState";

interface DashboardSummary {
  id: string;
  title: string;
  status: "active" | "archived";
  moduleCount: number;
  latestVersion: number;
  updatedAt: string;
  refreshIntervalMinutes: number | null;
}

interface ListPayload {
  selectedDashboardId: string | null;
  dashboards: DashboardSummary[];
}

interface DeviceMatch {
  dashboardId: string;
  version: number;
}

interface StatusPayload {
  reachable: boolean;
  displayedMatch: DeviceMatch | null;
  storedMatch: DeviceMatch | null;
}

export default function DashboardsPage() {
  const router = useRouter();
  const toast = useToast();
  const device = useDeviceState();

  const [list, setList] = useState<ListPayload | null>(null);
  const [status, setStatus] = useState<(StatusPayload & Record<string, unknown>) | null>(
    null,
  );
  const [error, setError] = useState("");
  const [busy, setBusy] = useState(false);
  const [stamp, setStamp] = useState(() => Date.now());
  const [menuFor, setMenuFor] = useState<string | null>(null);
  const [templatesOpen, setTemplatesOpen] = useState(false);
  const [sendFor, setSendFor] = useState<DashboardSummary | null>(null);

  /*
   * Archived compositions are always fetched, and folded away in the markup.
   *
   * The old page had a "Show archived" checkbox that changed the *request*, so
   * opening the fold meant a round trip and the count beside the summary could
   * not be known until after it was opened. Archiving is rare and the extra
   * rows are three fields each; asking for them once and deciding in the
   * browser is both simpler and the only way the summary can say "Archived
   * (2)" before anything is expanded.
   */
  const load = useCallback(async () => {
    try {
      const [listing, deviceStatus] = await Promise.all([
        apiGet<ListPayload>("/api/dashboards?includeArchived=1"),
        apiGet<StatusPayload & Record<string, unknown>>("/api/device/status").catch(
          () => null,
        ),
      ]);
      setList(listing);
      setStatus(deviceStatus);
      // The shell's chip lives off whatever a page already read, so handing
      // this over means the header costs no second knock at a battery-powered
      // panel's door. See src/ui/useDeviceState.ts.
      if (deviceStatus) publishDeviceStatus(deviceStatus);
      setError("");
    } catch (caught) {
      setError(caught instanceof ApiError ? caught.message : "The tower did not answer");
    }
  }, []);

  useEffect(() => {
    void load();
  }, [load]);

  async function run(work: () => Promise<unknown>, done?: string): Promise<void> {
    setBusy(true);
    setError("");
    setMenuFor(null);
    try {
      await work();
      setStamp(Date.now());
      await load();
      if (done) toast(done);
    } catch (caught) {
      setError(caught instanceof ApiError ? caught.message : "The action failed");
    } finally {
      setBusy(false);
    }
  }

  const active = list?.dashboards.filter((d) => d.status === "active") ?? [];
  const archived = list?.dashboards.filter((d) => d.status === "archived") ?? [];

  /**
   * Which composition the panel is showing, if the tower can tell.
   *
   * `displayedMatch` is the device's own reported digest resolved through the
   * ledger — the device said "I am showing this frame" and the tower recognised
   * the frame. `storedMatch` is a frame the device has but has not painted yet.
   * `selected` is neither: it is only what the tower would send next. Three
   * different facts, three different badges, and conflating them is how a
   * gallery ends up claiming the panel shows something it does not.
   */
  function badgeFor(summary: DashboardSummary) {
    if (status?.displayedMatch?.dashboardId === summary.id) {
      return (
        <Badge
          kind="displayed"
          title="The device confirmed it is showing a version of this composition"
        >
          On the panel
        </Badge>
      );
    }
    if (status?.storedMatch?.dashboardId === summary.id) {
      return (
        <Badge kind="stored" title="Stored on the device, awaiting a panel refresh">
          Stored
        </Badge>
      );
    }
    if (list?.selectedDashboardId === summary.id) {
      return (
        <Badge kind="selected" title="The tower will send this one next">
          Selected
        </Badge>
      );
    }
    return null;
  }

  const reading = device.input
    ? {
        input: device.input,
        observed: device.observed ?? true,
        nextWakeLabel: device.nextWakeLabel ?? null,
        address: device.address ?? null,
        simulated: device.simulated ?? true,
      }
    : null;

  return (
    <>
      <div className="page-head">
        <h1>Compositions</h1>
        <Button
          variant="primary"
          onClick={() => setTemplatesOpen(true)}
          testId="new-composition"
        >
          + New composition
        </Button>
      </div>

      <ErrorNote>{error}</ErrorNote>

      {!list && !error && <PageLoading label="Reading the compositions." rows={2} />}

      {list && list.dashboards.length === 0 && (
        <PageEmpty testId="dashboards-empty">
          No compositions yet. Start one from a template: each of the five is a
          real layout of real modules, and everything stays editable.
        </PageEmpty>
      )}

      <div className="card-grid" data-testid="dashboard-grid">
        {active.map((summary) => {
          const onPanel = status?.displayedMatch?.dashboardId === summary.id;
          return (
            <article className="dash-card" key={summary.id} data-testid={`dash-${summary.id}`}>
              {/*
                The picture is the button. A composition is recognised by what
                it looks like, so the thing a person reaches for should be the
                thing they recognised — not a small "Edit" under it. The
                preview is a real server render of this document at the four
                device pigments; `t=` defeats the browser cache so an edit made
                seconds ago is not shown as it was.
              */}
              <button
                type="button"
                className="dash-thumb"
                onClick={() => router.push(`/dashboards/${summary.id}/edit`)}
                aria-label={`Open ${summary.title} in the editor`}
                data-testid={`open-${summary.id}`}
              >
                {/* eslint-disable-next-line @next/next/no-img-element */}
                <img
                  src={`/api/dashboards/${summary.id}/preview?live=1&t=${stamp}`}
                  alt={`Preview of ${summary.title}`}
                  width={400}
                  height={300}
                  loading="lazy"
                />
                {badgeFor(summary)}
              </button>

              <div className="dash-body">
                <div className="dash-title-row">
                  <h3>{summary.title}</h3>
                  <span className="meta">edited {relativeTime(summary.updatedAt)}</span>
                </div>
                <span className="meta">
                  v{summary.latestVersion} · {summary.moduleCount} modules
                  {summary.refreshIntervalMinutes !== null && (
                    <> · auto every {summary.refreshIntervalMinutes} min</>
                  )}
                </span>

                <div className="actions">
                  {onPanel ? (
                    /*
                      Not a button that does nothing: a statement that happens
                      to occupy the button's place so the cards stay aligned.
                      Pressing "Show on panel" for the frame already on the
                      panel would open the send flow only to reach the dedup
                      screen, which is a worse way to learn the same fact.
                    */
                    <span
                      className="btn btn-default btn-grow btn-on-panel"
                      aria-disabled="true"
                      data-testid={`on-panel-${summary.id}`}
                    >
                      On the panel ✓
                    </span>
                  ) : (
                    <Button
                      variant="primary"
                      className="btn-grow"
                      disabled={busy}
                      onClick={() => {
                        /*
                          Two things, and only one of them touches the device.
                          Selecting is a tower-side preference — "this is what
                          goes next", which is also what the automatic refresh
                          follows — and is what pressing this button means
                          whether or not the send is then confirmed. The review
                          opens straight away rather than after the PATCH
                          lands, because a dialog that appears a round trip
                          after the press reads as a misfire.
                        */
                        setSendFor(summary);
                        void run(() =>
                          apiSend(`/api/dashboards/${summary.id}`, "PATCH", {
                            selected: true,
                          }),
                        );
                      }}
                      testId={`select-${summary.id}`}
                    >
                      Show on panel
                    </Button>
                  )}
                  <Link className="btn btn-default" href={`/dashboards/${summary.id}/edit`}>
                    Edit
                  </Link>
                  {/*
                    Labelled, because an unlabelled "..." announces nothing to
                    a screen reader. The row it opens replaces the floating
                    panel the old menu used: a panel positioned over a card
                    that the list reloads underneath is a menu whose buttons
                    refer to a state that has moved.
                  */}
                  <Button
                    className="btn-icon"
                    ariaLabel={`More actions for ${summary.title}`}
                    ariaPressed={menuFor === summary.id}
                    disabled={busy}
                    onClick={() =>
                      setMenuFor((value) => (value === summary.id ? null : summary.id))
                    }
                    testId={`more-${summary.id}`}
                  >
                    ⋯
                  </Button>
                </div>

                {menuFor === summary.id && (
                  <div className="more-row" data-testid={`more-panel-${summary.id}`}>
                    <Button
                      disabled={busy}
                      onClick={() =>
                        void run(
                          () =>
                            apiSend(`/api/dashboards/${summary.id}/duplicate`, "POST", {}),
                          `Duplicated “${summary.title}”`,
                        )
                      }
                    >
                      Duplicate
                    </Button>
                    <Button
                      disabled={busy}
                      onClick={() =>
                        void run(
                          () =>
                            apiSend(`/api/dashboards/${summary.id}`, "PATCH", {
                              status: "archived",
                            }),
                          "Archived — nothing on the panel changed",
                        )
                      }
                    >
                      Archive
                    </Button>
                  </div>
                )}
              </div>
            </article>
          );
        })}
      </div>

      {archived.length > 0 && (
        <details className="fold" style={{ marginTop: 26 }} data-testid="archived-fold">
          <summary data-testid="show-archived">Archived ({archived.length})</summary>
          <div className="archived-list">
            {archived.map((summary) => (
              <div
                className="archived-row"
                key={summary.id}
                data-testid={`dash-${summary.id}`}
              >
                <span>{summary.title}</span>
                <Button
                  disabled={busy}
                  onClick={() =>
                    void run(
                      () =>
                        apiSend(`/api/dashboards/${summary.id}`, "PATCH", {
                          status: "active",
                        }),
                      `Restored “${summary.title}”`,
                    )
                  }
                  testId={`restore-${summary.id}`}
                >
                  Restore
                </Button>
              </div>
            ))}
          </div>
          <p className="mono-note" style={{ paddingTop: "var(--pad-2)" }}>
            Archiving hides a composition from this gallery and nothing else. It
            keeps every version, and it never touches what the panel is showing.
          </p>
        </details>
      )}

      <TemplatePicker
        open={templatesOpen}
        onClose={() => setTemplatesOpen(false)}
        onCreated={(name) => toast(`Started from “${name}” — saved`)}
      />

      <SendFlowDialog
        open={sendFor !== null}
        dashboardId={sendFor?.id ?? null}
        dashboardTitle={sendFor?.title ?? ""}
        device={reading}
        onClose={() => setSendFor(null)}
        onSettled={() => {
          setStamp(Date.now());
          void load();
        }}
      />

      {list && list.dashboards.length > 0 && (
        <p className="field-hint" style={{ marginTop: "var(--pad-4)" }}>
          The picture on each card is the real renderer, at the panel&rsquo;s four
          pigments, from the saved document. Nothing here is an approximation —
          it is what the panel would show. See{" "}
          <Link href="/overview">Overview</Link> for what it is showing now.
        </p>
      )}
    </>
  );
}
