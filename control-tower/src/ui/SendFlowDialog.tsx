"use client";

import Link from "next/link";
import { useCallback, useEffect, useState } from "react";
import { NO_REMOTE_WAKE_NOTE, deriveDeviceState } from "@/core/power";
import { ApiError, apiGet, apiSend, csrfToken } from "./api";
import { Button } from "./components";
import { Dialog } from "./Dialog";
import { SEND_STAGES, stageIndexFor } from "./sendStages";
import type { DeviceReading } from "./useDeviceState";

/**
 * Showing a composition on the panel, from the review to the verdict.
 *
 * This dialog is the one place in the product where a person deliberately
 * reaches the hardware, so every screen in it is shaped by one rule: **it
 * never says something happened that the tower has not been told happened.**
 *
 * Concretely that means:
 *
 *  - The review shows the frame that will actually be sent, rendered by a dry
 *    run through the same pipeline the send uses, so "this exact image" is a
 *    statement about bytes rather than a promise about intent.
 *  - The five progress stages are lit from the push ledger, not from a timer.
 *    `GET /api/device/push` is polled while the POST is in flight and reports
 *    the transitions the pipeline has actually appended: `pending` lights the
 *    first stage, `sent` lights the next two (the device acknowledged storing
 *    the frame and returned a sequence number), and the last two stay unlit
 *    until the device itself says the panel is displaying it. A stage that
 *    advanced on a `setTimeout` would be a progress bar for something
 *    happening on a device the browser cannot see, which is the exact lie this
 *    product exists not to tell.
 *  - "Displayed — confirmed by the device" is only ever shown for the
 *    `verified_displayed` outcome, and the refresh time beside it is the
 *    measured `panelMs` from that push, not an average and not the ~25 s the
 *    review quotes.
 *  - `uncertain` gets its own screen. The prototype has no such state, because
 *    a prototype's device always answers; a real one sometimes does not, and
 *    the honest word for "we sent it and never heard back" is not "done" and
 *    not "failed".
 *
 * The typed PUSH confirmation is kept for a real device and is absent for the
 * mock, which matches what the server enforces: `REAL_PUSH_CONFIRMATION` is
 * required by the route whenever `deviceMode` is real, so a dialog that did
 * not ask for it would simply fail at the end instead of at the start.
 */

type Step =
  | "preflight"
  | "blocked"
  | "queued-blocked"
  | "review"
  | "progress"
  | "done"
  | "dedup"
  | "queued"
  | "failed"
  | "uncertain";

interface PushLine {
  state: string;
  at: string;
  seq: number | null;
  panelMs: number | null;
  detail: string | null;
}

interface PushRecordLite {
  pushId: string;
  state: string;
  first: { dashboardTitle: string; version: number };
  latest: PushLine;
}

interface LedgerView {
  blocking: PushRecordLite | null;
  lastPush: PushRecordLite | null;
}

interface DryRun {
  sha256: string;
  wouldDedup: boolean;
  bytes: number;
}

interface PushResponse {
  result: {
    outcome: string;
    detail: string;
    panelMs: number | null;
    seq: number | null;
  };
}

/**
 * Which screen each pipeline outcome ends on.
 *
 * One place, and it is total over `PushOutcome`. Six outcomes, six screens,
 * and no two of them share one: "held, nothing sent", "identical, nothing
 * sent", "sent and confirmed" and "sent and never confirmed" are four
 * different things that a single banner with the word swapped would flatten
 * into one.
 */
const STEP_FOR_OUTCOME: Record<string, Step> = {
  verified_displayed: "done",
  would_dedup: "dedup",
  queued: "queued",
  blocked: "blocked",
  uncertain: "uncertain",
  failed: "failed",
};

/** How often the ledger is re-read while a push is in flight. */
const LEDGER_POLL_MS = 1200;

export function SendFlowDialog({
  open,
  dashboardId,
  dashboardTitle,
  device,
  onClose,
  onSettled,
}: {
  open: boolean;
  dashboardId: string | null;
  dashboardTitle: string;
  device: DeviceReading | null;
  onClose: () => void;
  /** Called once the panel may have changed, so the page can re-read. */
  onSettled: () => void;
}) {
  const [step, setStep] = useState<Step>("preflight");
  const [dry, setDry] = useState<DryRun | null>(null);
  const [blocking, setBlocking] = useState<PushRecordLite | null>(null);
  const [ledgerState, setLedgerState] = useState<string | null>(null);
  const [result, setResult] = useState<PushResponse["result"] | null>(null);
  const [error, setError] = useState("");
  const [typed, setTyped] = useState("");
  const [stamp, setStamp] = useState(() => Date.now());

  const simulated = device?.simulated ?? true;
  const state = device ? deriveDeviceState(device.input) : null;
  const asleep = state?.state === "asleep" || state?.state === "pending";

  // ---------------------------------------------------------- preflight --

  const preflight = useCallback(async () => {
    if (!dashboardId) return;
    setStep("preflight");
    setError("");
    setTyped("");
    setResult(null);
    setLedgerState(null);
    try {
      // The ledger first. A push that is already outstanding refuses this one
      // at the server with a 409, and finding that out *after* somebody has
      // reviewed a frame and typed PUSH is a worse experience than saying so
      // up front — which is also the only screen that can point at where the
      // outstanding one gets resolved.
      const view = await apiGet<LedgerView>("/api/device/push");
      if (view.blocking) {
        setBlocking(view.blocking);
        setStep(view.blocking.state === "queued" ? "queued-blocked" : "blocked");
        return;
      }
      setBlocking(null);

      const preview = await apiSend<DryRun>("/api/device/push", "POST", {
        dashboardId,
        dryRun: true,
      });
      setDry(preview);
      setStamp(Date.now());
      setStep("review");
    } catch (caught) {
      setError(
        caught instanceof ApiError
          ? caught.message
          : "The tower could not prepare this frame",
      );
      setStep("failed");
    }
  }, [dashboardId]);

  useEffect(() => {
    if (!open) return;
    void preflight();
  }, [open, preflight]);

  // ------------------------------------------------------------- sending --

  /*
   * Poll the ledger while the POST is in flight.
   *
   * Read-only, local, and it stops the moment the step is no longer
   * `progress`. The interval is deliberately not tied to the push's own poll
   * schedule: this is a browser asking a local file what it says, and the
   * device is not involved either way.
   */
  useEffect(() => {
    if (step !== "progress") return;
    let live = true;
    const read = async (): Promise<void> => {
      try {
        const view = await apiGet<LedgerView>("/api/device/push");
        if (!live) return;
        const record = view.blocking ?? view.lastPush;
        if (record) setLedgerState(record.state);
      } catch {
        // A failed poll is not a failed push. The POST is still in flight and
        // its answer is the one that decides the outcome; all this loses is
        // the animation of a stage, which is exactly the right thing to lose.
      }
    };
    void read();
    const timer = setInterval(() => void read(), LEDGER_POLL_MS);
    return () => {
      live = false;
      clearInterval(timer);
    };
  }, [step]);

  async function confirmSend(): Promise<void> {
    if (!dashboardId) return;
    setError("");
    setStep("progress");
    setLedgerState("pending");
    try {
      /*
       * A raw fetch, not `apiSend`, and the reason is the contract rather than
       * convenience. Four of the six outcomes come back on a non-2xx status —
       * `would_dedup` and `blocked` on 409, `failed` on 502, `queued` on 202 —
       * and every one of them carries a full `result` object saying which.
       * None of them is an error: a frame held for a sleeping device is the
       * designed behaviour of this product, not a failure to be caught. Branching
       * on the status code would mean re-deriving from an HTTP number a thing
       * the body already states, and mapping "409" back to one of two different
       * meanings by reading its prose.
       */
      const response = await fetch("/api/device/push", {
        method: "POST",
        headers: {
          "content-type": "application/json",
          "x-csrf-token": csrfToken(),
        },
        cache: "no-store",
        body: JSON.stringify({
          dashboardId,
          force: dry?.wouldDedup ?? false,
          confirm: simulated ? undefined : "PUSH",
        }),
      });

      const body = (await response.json().catch(() => null)) as
        | PushResponse
        | { error?: string; detail?: string }
        | null;

      if (body && "result" in body && body.result) {
        setResult(body.result);
        const next = STEP_FOR_OUTCOME[body.result.outcome];
        if (next) {
          setStep(next);
        } else {
          // An outcome this dialog has never heard of. Failing closed is the
          // only safe direction: the one thing it must not do is show the
          // "confirmed" screen for a verdict it cannot read.
          setError(`The tower reported an outcome this page does not know: ${body.result.outcome}`);
          setStep("failed");
        }
        return;
      }

      // No result object at all: a guard refused the request before the
      // pipeline ran — no selection, no confirmation word, no session.
      const shaped = (body ?? {}) as { detail?: string };
      setError(shaped.detail ?? `The tower answered ${response.status}`);
      setStep("failed");
    } catch {
      setError("The tower did not answer");
      setStep("failed");
    } finally {
      // Whatever the verdict, the panel may have changed and the page behind
      // this dialog is now describing a device it has not re-read.
      onSettled();
    }
  }

  // ------------------------------------------------------------ rendering --

  if (!open) return null;

  const stageIndex = stageIndexFor(ledgerState);
  const nextWake = device?.nextWakeLabel ?? null;

  const title =
    step === "review" || step === "preflight"
      ? "Show on the panel"
      : step === "progress"
        ? "Sending…"
        : step === "blocked" || step === "queued-blocked"
          ? "Can't send yet"
          : "Result";

  /*
   * The backdrop and the close button are both disabled during `progress`, and
   * for once that is not a dark pattern. The POST is in flight and holds the
   * only reference to its own outcome; closing the dialog would not cancel the
   * push — the frame is already on its way to the panel — it would just throw
   * away the answer. There is nothing to escape to.
   */
  const dismissable = step !== "progress";

  return (
    <Dialog
      open={open}
      title={title}
      id="send-flow"
      testId="send-flow"
      onClose={dismissable ? onClose : () => {}}
      head={
        <Button
          className="modal-close"
          onClick={onClose}
          disabled={!dismissable}
          ariaLabel="Close"
          testId="send-flow-close"
        >
          ✕
        </Button>
      }
      footer={null}
    >
      {step === "preflight" && (
        <p className="empty" data-testid="send-preflight">
          Rendering the frame the device would receive…
        </p>
      )}

      {step === "review" && (
        <div data-testid="send-review">
          <div className="send-review">
            <div className="send-thumb">
              {/*
                A PNG from the server, not a client canvas: the dry run has
                already rendered these exact bytes through the real pipeline,
                and this asks the preview route for the same document so what
                is on screen and what is in the digest below it came from one
                renderer. `t=` defeats the browser cache, which would otherwise
                show the previous version of a composition edited seconds ago.
              */}
              <img
                src={`/api/dashboards/${dashboardId}/preview?live=1&t=${stamp}`}
                alt={`The frame that would be sent from ${dashboardTitle}`}
                width={400}
                height={300}
                data-testid="send-frame"
              />
            </div>
            <div className="send-copy">
              <p>This exact image will be sent to the panel.</p>
              <p className="mono-note">
                400 × 300 · 4 colours · the panel will blink while it refreshes
                (~25 s) — that&rsquo;s normal.
              </p>
              {dry && (
                <p className="mono-note" data-testid="send-digest">
                  frame digest {dry.sha256.slice(0, 8)}…
                  {dry.sha256.slice(-4)} · {dry.bytes} bytes
                </p>
              )}
              {dry?.wouldDedup && (
                <p className="mono-note" data-testid="send-would-dedup">
                  The panel already shows exactly this image. Sending it again
                  would blink the panel and spend battery for no change.
                </p>
              )}
              {asleep && (
                <p className="send-note-queued" data-testid="send-asleep-note">
                  The device is asleep — it can&rsquo;t be woken over Wi-Fi.
                  Your image will be queued and applied at the next wake
                  {nextWake ? `, around ${nextWake}` : ""}.
                </p>
              )}
            </div>
          </div>

          {/*
            The typed word, on the review rather than in a second dialog.
            Stacking a confirmation dialog on top of this one would be two
            overlays, which src/ui/overlay.ts refuses; and the thing being
            confirmed — the picture — is already on this screen, which is where
            a confirmation should be.
          */}
          {!simulated && (
            <label className="field" style={{ marginTop: "var(--pad-3)" }}>
              <span>
                Type PUSH to confirm a send to the real device
              </span>
              <input
                value={typed}
                onChange={(event) => setTyped(event.target.value)}
                data-testid="send-confirm-word"
                autoComplete="off"
                data-dialog-autofocus=""
              />
            </label>
          )}

          <div className="modal-actions" style={{ padding: "var(--pad-3) 0 0" }}>
            <Button onClick={onClose} testId="send-cancel">
              Cancel
            </Button>
            <Button
              variant="primary"
              onClick={() => void confirmSend()}
              disabled={!simulated && typed !== "PUSH"}
              testId="send-confirm"
            >
              {asleep ? "Queue for next wake" : "Confirm and send"}
            </Button>
          </div>
        </div>
      )}

      {step === "progress" && (
        <ul className="stage-list" data-testid="send-progress">
          {SEND_STAGES.map((stage, index) => {
            const stageState =
              index < stageIndex ? "done" : index === stageIndex ? "now" : "todo";
            return (
              <li
                className="stage"
                data-state={stageState}
                data-testid={`send-stage-${index}`}
                key={stage.name}
              >
                <span className="stage-dot" aria-hidden="true" />
                <span style={{ flex: 1 }}>
                  <span className="stage-name">{stage.name}</span>
                  {stageState === "now" && stage.note !== "" && (
                    <span className="stage-note">{stage.note}</span>
                  )}
                </span>
                <span className="stage-stamp" aria-hidden="true">
                  {stageState === "done" ? "✓" : stageState === "now" ? "…" : ""}
                </span>
              </li>
            );
          })}
        </ul>
      )}

      {step === "done" && (
        <div className="send-result" data-testid="send-done">
          <div className="send-glyph" data-tone="done" aria-hidden="true">
            ✓
          </div>
          <h3>Displayed — and the device confirmed it.</h3>
          <span className="mono-meta">
            {result?.panelMs
              ? `refresh took ${(result.panelMs / 1000).toFixed(1)} s`
              : "refresh time not reported"}
            {result?.seq !== null && result?.seq !== undefined
              ? ` · sequence ${result.seq}`
              : ""}
          </span>
          <Button variant="primary" onClick={onClose} testId="send-close-done">
            Done
          </Button>
        </div>
      )}

      {step === "dedup" && (
        <div className="send-result" data-testid="send-dedup">
          <div className="send-glyph" aria-hidden="true">
            =
          </div>
          <h3>Nothing sent — the panel already shows exactly this.</h3>
          <p>
            A refresh would blink the panel and spend battery for an identical
            image, so it was skipped.
          </p>
          <Button onClick={onClose} testId="send-close-dedup">
            OK
          </Button>
        </div>
      )}

      {step === "queued" && (
        <div className="send-result" data-testid="send-queued">
          <div className="send-glyph" data-tone="queued" aria-hidden="true">
            ⏸
          </div>
          <h3>Queued for the next wake.</h3>
          <p>
            Nothing has been sent yet. {NO_REMOTE_WAKE_NOTE}
            {nextWake
              ? ` It is next expected to be reachable around ${nextWake}.`
              : ""}
          </p>
          <Button onClick={onClose} testId="send-close-queued">
            OK
          </Button>
        </div>
      )}

      {step === "uncertain" && (
        <div data-testid="send-uncertain">
          <div className="send-problem">
            <h3>Sent, but never confirmed.</h3>
            <p>
              The frame went to the device and the device did not report back
              within the time the firmware allows for a refresh. It may or may
              not be on the panel right now — the tower will not guess.
            </p>
            <p>{result?.detail}</p>
          </div>
          <p className="mono-note" style={{ marginTop: "var(--pad-2)" }}>
            Sending is paused until this is resolved. Resolve it from the
            Overview: re-check the device, or accept the outcome as unknown.
            Both are recorded.
          </p>
          <div className="modal-actions" style={{ padding: "var(--pad-3) 0 0" }}>
            <Link className="btn btn-primary" href="/overview" onClick={onClose}>
              Go to Overview
            </Link>
          </div>
        </div>
      )}

      {step === "failed" && (
        <div data-testid="send-failed">
          <div className="send-problem">
            <h3>Couldn&rsquo;t reach the device.</h3>
            <p>
              The panel still shows its last image — e-paper keeps it without
              power, so nothing has been lost. Check the device&rsquo;s power
              and Wi-Fi.
            </p>
            {(error || result?.detail) && (
              <p className="mono-note" data-testid="send-failed-detail">
                {error || result?.detail}
              </p>
            )}
          </div>
          <div className="modal-actions" style={{ padding: "var(--pad-3) 0 0" }}>
            <Link className="btn" href="/diagnostics" onClick={onClose}>
              Open diagnostics
            </Link>
            <Button variant="dark" onClick={onClose} testId="send-close-failed">
              Close
            </Button>
          </div>
        </div>
      )}

      {step === "blocked" && (
        <div data-testid="send-blocked">
          <div className="send-problem">
            <h3>A previous send is still unresolved.</h3>
            <p>
              The device never confirmed the last frame
              {blocking
                ? ` (${blocking.first.dashboardTitle} v${blocking.first.version})`
                : ""}
              . Sending another now could cause a double refresh. Resolve it
              first from the Overview.
            </p>
            {error && <p className="mono-note">{error}</p>}
          </div>
          <div className="modal-actions" style={{ padding: "var(--pad-3) 0 0" }}>
            <Link className="btn btn-primary" href="/overview" onClick={onClose}>
              Go to Overview
            </Link>
          </div>
        </div>
      )}

      {step === "queued-blocked" && (
        <div data-testid="send-queued-blocked">
          <div className="send-problem">
            <h3>Something else is already queued.</h3>
            <p>
              {blocking
                ? `${blocking.first.dashboardTitle} v${blocking.first.version} is rendered and waiting for the device to wake. `
                : ""}
              Two frames racing for the same wake window would leave nobody able
              to say which one the panel ends up showing, so this one is
              refused. Withdraw the queued frame from the Overview and try
              again — it takes one press and sends nothing.
            </p>
          </div>
          <div className="modal-actions" style={{ padding: "var(--pad-3) 0 0" }}>
            <Link className="btn btn-primary" href="/overview" onClick={onClose}>
              Go to Overview
            </Link>
          </div>
        </div>
      )}
    </Dialog>
  );
}
