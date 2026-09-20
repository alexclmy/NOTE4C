"use client";

import {
  DEVICE_ACTION_COPY,
  INTERACTIVE_MINUTES,
  INTERACTIVE_REQUEST_NOTE,
  deriveDeviceState,
  describeWhatHappensNext,
  type DeviceStateInput,
} from "@/core/power";
import { useState } from "react";
import { Button, Card, MonoLabel } from "./components";
import { Disclosure } from "./Disclosure";
import { useRegisterPageDeviceState } from "./pageDeviceState";

/**
 * "Now": what the device is doing, and the two things you can do about it.
 *
 * This was a one-line strip across the top of Overview and Device. It is a
 * card now, and the reason is not decoration: the strip carried a badge, a
 * reason, an address, a wake time and two buttons on one line, which on a
 * phone wrapped into five ragged rows and on a desktop read as a toolbar
 * rather than as the most important fact on the page.
 *
 * The word and the sentence both come from `deriveDeviceState` and from
 * nowhere else — the same call the header chip makes — so the two renderings
 * of the device's state cannot disagree. That was the original defect this
 * component was written for: Overview showed a red UNREACHABLE for a device
 * sleeping exactly as designed while Device showed a yellow "Not answering",
 * and a reader moving between them had to work out that both meant the same
 * thing.
 *
 * It carries the one request that most often precedes touching the device:
 * asking it to stay awake. That is the thing you want before you walk over to
 * the panel, and it used to be six buttons deep inside one card of one page.
 */
export function StateStrip({
  input,
  address,
  nextWakeLabel,
  batteryLine,
  onReadAgain,
  checking = false,
  onInteractive,
  busy,
  testId = "state-strip",
}: {
  input: DeviceStateInput;
  address: string | null;
  nextWakeLabel: string | null;
  /** Already worded by the caller, which is the page that read the battery. */
  batteryLine?: string | null;
  onReadAgain?: () => void;
  /**
   * A read somebody pressed for is in flight.
   *
   * Separate from `busy`, which covers writes. It is here because this read is
   * now allowed to take real time: when this machine is refusing connections
   * to the panel out of its own routing table — a state it holds for about
   * twenty seconds after one failed address resolution — the only way to get a
   * true answer is to wait it out. A button that looks inert for twenty
   * seconds gets pressed four more times; one that says what it is doing does
   * not.
   */
  checking?: boolean;
  /** Given the window in minutes, so the choice below means something. */
  onInteractive?: (minutes: number) => void;
  busy?: boolean;
  testId?: string;
}) {
  const reading = deriveDeviceState(input);
  /*
   * Which window the next request asks for. It is a choice about *now* —
   * somebody is about to walk over to the device — so it is not persisted
   * anywhere. Fifteen minutes is the default because it is about the length of
   * one trip to the panel and back.
   */
  const [minutes, setMinutes] = useState<number>(15);

  // Tell the shell this page is already showing the state, so nothing in the
  // chrome stacks a second copy of it. See src/ui/pageDeviceState.ts.
  useRegisterPageDeviceState();

  return (
    <div data-testid={testId} data-device-state={reading.state} data-state={reading.state}>
    <Card title="Now" variant="plain">
      <div className="now-line" data-device-state={reading.state}>
        <span className="chip-dot now-dot" aria-hidden="true" />
        <strong data-testid={`${testId}-badge`}>{reading.label}</strong>
      </div>

      <p style={{ margin: "10px 0 0", fontSize: "var(--size-14)", lineHeight: 1.6 }}>
        {reading.reason}
      </p>
      <p style={{ margin: "8px 0 0", fontSize: "var(--size-14)", lineHeight: 1.6 }}>
        {DEVICE_ACTION_COPY[reading.state]}{" "}
        {describeWhatHappensNext(reading.state, nextWakeLabel)}
      </p>

      {/*
        The facts, in one mono row. Address included, and it is the tower's
        configured address rather than anything the device announced — which is
        why it sits beside a battery reading the device did report and is worded
        as a plain value rather than as a claim about what answered.
      */}
      <div className="now-facts">
        {batteryLine && <span>{batteryLine}</span>}
        {nextWakeLabel && <span>next wake ~{nextWakeLabel}</span>}
        {address && <span className="num">{address}</span>}
      </div>

      <div className="card-actions" style={{ marginTop: "var(--pad-3)" }}>
        {onReadAgain && (
          <Button
            onClick={onReadAgain}
            disabled={busy || checking}
            testId="refresh"
          >
            {checking ? "Checking…" : "Check now"}
          </Button>
        )}
        {onInteractive && (
          <>
            <Button
              onClick={() => onInteractive(minutes)}
              disabled={busy}
              testId={`${testId}-interactive`}
            >
              Interactive {minutes} min
            </Button>
            {/*
              The caveat, in a disclosure rather than a `title`. What this
              button does and does not do — applied now if the device is awake,
              held if it is not, and never a wake — is the product's own
              contract, and a `title` publishes it to a hovering mouse and to
              nobody else. See INTERACTIVE_REQUEST_NOTE in src/core/power.ts.
            */}
            <Disclosure
              text={INTERACTIVE_REQUEST_NOTE}
              label="What asking for interactive does"
              testId={`${testId}-interactive-why`}
            />
          </>
        )}
      </div>

      {onInteractive && (
        <div style={{ marginTop: "var(--pad-2)" }}>
          <MonoLabel>Interactive window</MonoLabel>
          <div className="chip-row" style={{ marginTop: 8 }}>
            {INTERACTIVE_MINUTES.map((option) => (
              <Button
                key={option}
                ariaPressed={minutes === option}
                onClick={() => setMinutes(option)}
                testId={`power-window-${option}`}
              >
                {option} min
              </Button>
            ))}
          </div>
        </div>
      )}
    </Card>
    </div>
  );
}
