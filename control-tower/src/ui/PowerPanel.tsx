"use client";

import { useState, type ReactNode } from "react";
import {
  DEEP_SAVER_WAKE_MIN,
  DEFAULT_WAKE_INTERVAL_MIN,
  MODE_COPY,
  NO_REMOTE_WAKE_NOTE,
  WAKE_INTERVAL_PRESETS,
  energyChoice,
  type DevicePower,
  type PowerIntent,
  type PowerMode,
} from "@/core/power";
import { Badge, Banner, Button, Card, MonoLabel } from "@/ui/components";
import { ConfirmDialog } from "@/ui/Dialog";


/**
 * Energy: how much of the time the device is listening, and what that costs.
 *
 * The design constraint that shapes every part of this card: **the device is
 * unreachable most of the time, and nothing here can change that.** A control
 * surface that hid that fact would produce a user who believes their clicks
 * did something, and a device that quietly did not do it.
 *
 * So:
 *  - Not answering is never drawn as an error. It is the normal state.
 *  - There is no "wake device" button, because there is no such operation.
 *    The note explaining why is shown where that button would have been.
 *  - A request that could not be delivered is shown as *pending*, with what it
 *    is waiting for and how to make it happen now (the button on the device).
 *  - Nothing here promises a battery life in days. The measurements do not
 *    exist yet, and the card says that instead of inventing a figure.
 *
 * What changed in the refit is the shape. The three firmware modes used to be
 * a segmented control plus a separate red button, which put "Always awake" —
 * the one choice that costs two orders of magnitude of battery life — either at
 * the same rank as the others or in a fence with the destructive actions. They
 * are now three cards that each state their own cost in the same sentence as
 * their name, because the choice being made is a trade and a list of equal
 * options hides the trade.
 *
 * The three cards are not three firmware modes. `always_on` is one, and the
 * other two are both `auto_saver` at different wake intervals — which is the
 * truth about the hardware: sleeping more and sleeping less is one mode with a
 * number, and the number is what the chips below set.
 */

export interface PowerPanelProps {
  reachable: boolean;
  /**
   * Whether the device was asked at all.
   *
   * The note at the foot of this card says a change made here will be applied
   * "the next time it is actually there", which reads as a claim that it is
   * not there now. On the first paint nothing has been asked, so that claim is
   * not the tower's to make — and it would be visibly wrong for the panel this
   * product's own incident was about, which was awake and answering while the
   * interface said otherwise.
   */
  observed?: boolean;
  power: DevicePower | null;
  powerSupported: boolean | null;
  intent: PowerIntent | null;
  intentDetail: string;
  batteryUnavailableReason: string | null;
  reachabilityNote: string;
  nextWake: { at: string; estimated: boolean } | null;
  deviceLastSeenAt?: string | null;
  busy: boolean;
  onSetMode: (mode: PowerMode, options?: { wakeIntervalMinutes?: number }) => void | Promise<void>;
  onCancelIntent: () => void | Promise<void>;
  /**
   * Try to deliver the pending request now.
   *
   * This button exists because delivery moved off the status read. Reading the
   * page no longer writes to the device, so the tower needs somewhere explicit
   * to say "try it now" for a user who has just walked over and pressed the
   * button on the device and does not want to wait for the scheduler.
   */
  onReconcile: () => void | Promise<void>;
  /**
   * Restart, and anything else whose consequence is on the hardware.
   *
   * Supplied by the page that owns the settings registry, because that is
   * where the capability gating lives: a firmware that advertises no restart
   * route must not be offered a restart button.
   */
  dangerActions?: ReactNode;
}

function wakeLabel(minutes: number): string {
  return minutes >= 60 ? `${minutes / 60} h` : `${minutes} min`;
}

function formatAgo(iso: string, now: number): string {
  const then = new Date(iso).getTime();
  if (Number.isNaN(then)) return "an unknown time ago";
  const minutes = Math.max(0, Math.round((now - then) / 60_000));
  if (minutes < 1) return "just now";
  if (minutes < 60) return `${minutes} minute${minutes === 1 ? "" : "s"} ago`;
  const hours = Math.round(minutes / 60);
  return `${hours} hour${hours === 1 ? "" : "s"} ago`;
}

export function PowerPanel(props: PowerPanelProps) {
  const {
    reachable,
    observed = true,
    power,
    powerSupported,
    intent,
    intentDetail,
    batteryUnavailableReason,
    nextWake,
    busy,
    onSetMode,
    onCancelIntent,
    onReconcile,
    dangerActions,
  } = props;

  const [confirmAlwaysOn, setConfirmAlwaysOn] = useState(false);
  const now = Date.now();

  if (powerSupported === false) {
    return (
      <Card title="Energy" id="power" variant="plain">
        <Banner tone="info" testId="power-unsupported">
          This device&rsquo;s firmware does not advertise the hybrid power
          contract, so the tower has no energy controls to offer for it. Nothing
          here is hidden; there is nothing to show.
        </Banner>
        {dangerActions && (
          <div className="danger-zone" data-testid="power-danger-zone">
            <h3>Actions with consequences</h3>
            <div className="danger-actions">{dangerActions}</div>
          </div>
        )}
      </Card>
    );
  }

  const choice = energyChoice(power);
  const interval = power?.wake_interval_min ?? DEFAULT_WAKE_INTERVAL_MIN;
  const pending = intent !== null && intent.appliedAt === null;

  const MODES: ReadonlyArray<{
    id: "ready" | "balanced" | "saver";
    name: string;
    desc: string;
    testId: string;
    danger?: boolean;
    onPick: () => void;
  }> = [
    {
      id: "ready",
      name: "Always ready",
      desc: "Stays connected. Sends land immediately. Drains the battery fastest.",
      testId: "power-set-always-on",
      danger: true,
      onPick: () => setConfirmAlwaysOn(true),
    },
    {
      id: "balanced",
      name: "Balanced",
      desc: `Wakes every ${wakeLabel(interval)}, listens for a while, sleeps again. A queued send waits that long at most.`,
      testId: "power-set-auto",
      onPick: () =>
        void onSetMode("auto_saver", {
          // A wake interval at or past the deep-saver figure is what makes a
          // device "Deep saver", so choosing Balanced from there has to bring
          // the number back with it or the card would select and the device
          // would not move.
          wakeIntervalMinutes:
            interval >= DEEP_SAVER_WAKE_MIN ? DEFAULT_WAKE_INTERVAL_MIN : interval,
        }),
    },
    {
      id: "saver",
      name: "Deep saver",
      desc: "Wakes twice a day. Best battery — a change can wait up to half a day before it reaches the panel.",
      testId: "power-set-saver",
      onPick: () =>
        void onSetMode("auto_saver", { wakeIntervalMinutes: DEEP_SAVER_WAKE_MIN }),
    },
  ];

  return (
    <Card title="Energy" id="power" variant="plain">
      <p className="field-hint" style={{ marginBottom: "var(--pad-3)" }}>
        More sleep means more battery — and longer waits before a change reaches
        the panel.
      </p>

      {/* A request the device has not heard yet. Not an error. */}
      {pending ? (
        <Banner tone="pending" testId="power-intent-pending">
          <span>
            <strong>Waiting to be applied:</strong>{" "}
            {MODE_COPY[intent.mode].label}
            {intent.mode === "interactive" && intent.interactiveMinutes !== null
              ? ` for ${intent.interactiveMinutes} minutes`
              : null}
            , requested {formatAgo(intent.requestedAt, now)}.{" "}
            {intentDetail || NO_REMOTE_WAKE_NOTE}
            {intent.lastAttemptError ? <> Last attempt: {intent.lastAttemptError}</> : null}
            {intent.attempts > 0 ? (
              <>
                {" "}
                Delivery has been attempted {intent.attempts} time
                {intent.attempts === 1 ? "" : "s"}.
              </>
            ) : null}
          </span>
          <Button
            onClick={() => void onReconcile()}
            disabled={busy}
            testId="power-reconcile"
            title="Ask the tower to try delivering this now. It only works if the device is awake — press the button on the device first."
          >
            Try now
          </Button>
          <Button
            onClick={() => void onCancelIntent()}
            disabled={busy}
            testId="power-cancel-intent"
          >
            Cancel this request
          </Button>
        </Banner>
      ) : null}

      <div className="mode-cards" role="group" aria-label="Energy mode">
        {MODES.map((mode) => (
          <button
            type="button"
            className="mode-card"
            key={mode.id}
            aria-pressed={choice === mode.id}
            disabled={busy}
            onClick={mode.onPick}
            data-testid={mode.testId}
          >
            <span className="mode-card-name">
              <span>{mode.name}</span>
              <span aria-hidden="true">{choice === mode.id ? "●" : "○"}</span>
            </span>
            <span className="mode-card-desc">{mode.desc}</span>
            {mode.danger && MODE_COPY.always_on.warning && (
              <span className="mode-card-warning">
                {MODE_COPY.always_on.warning}
              </span>
            )}
          </button>
        ))}
      </div>

      {/*
        The wake interval, as chips.
        Shown whatever the selected card is, because it is the number the
        device is actually running with and reading it should not require
        changing anything. Pressing one writes `auto_saver` with that interval,
        which is what "wake every N" means to the firmware.
      */}
      <div style={{ marginTop: "var(--pad-3)" }}>
        <MonoLabel>Wakes every</MonoLabel>
        <div className="chip-row" style={{ marginTop: 8 }}>
          {WAKE_INTERVAL_PRESETS.map((option) => (
            <Button
              key={option}
              ariaPressed={power !== null && interval === option}
              disabled={busy}
              onClick={() =>
                void onSetMode("auto_saver", { wakeIntervalMinutes: option })
              }
              testId={`power-wake-${option}`}
            >
              {wakeLabel(option)}
            </Button>
          ))}
        </div>
        <p className="mono-note" style={{ marginTop: "var(--pad-2)" }}>
          {power === null
            ? "The device has not been read since it was last awake, so the tower cannot say which interval it is running."
            : `The device reports it is waking every ${wakeLabel(interval)}.`}{" "}
          Real battery life is still being measured — no promises in days yet.
        </p>
      </div>

      {/* Battery, with the reason rather than a made-up number. */}
      <p className="mono-note" style={{ marginTop: "var(--pad-3)" }}>
        {power?.battery.percent !== null && power?.battery.percent !== undefined ? (
          <span data-testid="power-battery">
            battery {power.battery.percent}%
            {power.battery.mv !== null ? ` · ${power.battery.mv} mV` : ""}
            {power.charge.charging ? " · charging" : ""} · charger{" "}
            {power.charge.state.replace(/_/g, " ")}
          </span>
        ) : (
          <span data-testid="power-battery-unavailable">
            Battery not reported.{" "}
            {batteryUnavailableReason ??
              "The device has not been read since it was last awake."}
          </span>
        )}
      </p>

      {power && power.last_outcome === null ? (
        <p className="mono-note" data-testid="power-no-outcome">
          No update cycle has finished yet, so the device has nothing to report
          about its last one. This is what a device that has just booted looks
          like.
        </p>
      ) : null}

      {power && power.consecutive_failures > 0 ? (
        <Banner tone="attention" testId="power-failures">
          <span>
            The device has failed {power.consecutive_failures} update cycle
            {power.consecutive_failures === 1 ? "" : "s"} in a row
            {power.last_outcome
              ? ` (last outcome: ${power.last_outcome.replace(/_/g, " ")}`
              : ""}
            {power.last_outcome && power.budget_exhausted_phase
              ? `, which ran out during ${power.budget_exhausted_phase}`
              : ""}
            {power.last_outcome ? ")" : ""}. It is backing off between retries
            rather than waking repeatedly to fail, so the panel may be stale for
            longer than the wake interval.
          </span>
        </Banner>
      ) : null}

      {nextWake?.estimated && (
        <p className="mono-note">
          The next wake shown on this page is the tower&rsquo;s own arithmetic
          from the last time it saw the device, not the device&rsquo;s answer.
          It is wrong whenever the device backed off after a failed cycle.
        </p>
      )}

      {/*
        Where a "wake device" button would go, if one could exist. It cannot,
        and saying so here is the whole point: the alternative is a user who
        keeps looking for the control.
      */}
      <p className="mono-note" data-testid="power-no-remote-wake">
        {NO_REMOTE_WAKE_NOTE}
      </p>

      {dangerActions && (
        <div className="danger-zone" data-testid="power-danger-zone">
          <h3>Actions with consequences</h3>
          <p className="field-hint">
            Each of these changes what the hardware is doing. None of them can
            be undone from here.
          </p>
          <div className="danger-actions">{dangerActions}</div>
        </div>
      )}

      {/*
        What pressing anything above actually promises, when nobody is
        listening. Written as a line rather than behind a disclosure toggle
        because it is the answer to the first question a person has about a
        control on a device that is asleep most of the time, and a lone "i"
        glyph at the bottom of a card is not an invitation anybody accepts.
      */}
      {!reachable && (
        <p className="mono-note" style={{ marginTop: "var(--pad-2)" }}>
          {observed ? (
            <>
              <Badge kind="asleep" /> The device is not answering, so a change
              made here is recorded and applied the next time it is actually
              there to receive it. The tower never claims a change reached a
              device it could not reach.
            </>
          ) : (
            <>
              <Badge kind="uncertain" /> The tower has not read the device yet.
              A change made now is recorded either way, and applied the moment
              the device is there to receive it.
            </>
          )}
        </p>
      )}

      <ConfirmDialog
        open={confirmAlwaysOn}
        title="Keep the device awake all the time?"
        body={<p>{MODE_COPY.always_on.warning}</p>}
        confirmLabel="Keep it awake"
        onCancel={() => setConfirmAlwaysOn(false)}
        onConfirm={() => {
          setConfirmAlwaysOn(false);
          void onSetMode("always_on");
        }}
      />
    </Card>
  );
}
