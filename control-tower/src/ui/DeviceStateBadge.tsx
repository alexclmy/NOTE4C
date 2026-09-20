"use client";

import { deriveDeviceState, type DeviceStateInput } from "@/core/power";
import { Badge } from "./components";
import { Disclosure } from "./Disclosure";

/**
 * The device's state, in one word, from one function.
 *
 * Every page hands in the same reading and gets back the same word: there is
 * no second opinion to disagree with. The badge carries the sentence with it —
 * behind a disclosure rather than a `title`, so a phone can read the reason a
 * colour was chosen.
 */
export function DeviceStateBadge({
  input,
  showReason = false,
  testId = "device-state",
}: {
  input: DeviceStateInput;
  /** Print the sentence beside the badge instead of folding it away. */
  showReason?: boolean;
  testId?: string;
}) {
  const reading = deriveDeviceState(input);
  return (
    <span className="device-state" data-device-state={reading.state}>
      <Badge kind={reading.state} testId={testId}>
        {reading.label.toUpperCase()}
      </Badge>{" "}
      {showReason ? (
        <span className="device-state-reason" data-testid={`${testId}-reason`}>
          {reading.reason}
        </span>
      ) : (
        <Disclosure text={reading.reason} label="Why this state" testId={`${testId}-why`} />
      )}
    </span>
  );
}
