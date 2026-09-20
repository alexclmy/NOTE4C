"use client";

import Link from "next/link";
import { deriveDeviceState } from "@/core/power";
import { relativeTime } from "./api";
import type { DeviceReading } from "./useDeviceState";

/**
 * The device's state, in the header, on every page.
 *
 * This is the fact every other decision in this product depends on — whether a
 * change can reach the panel now, later, or not at all — and before the header
 * existed it could only be read by navigating to Device.
 *
 * The word comes from `deriveDeviceState` and from nowhere else, exactly as it
 * does in `DeviceStateBadge`: there is one translation of a reading into a
 * state in this product, and both renderings of it read the same function. The
 * colour of the dot comes from `data-device-state` in app.css, so the chip, the
 * Now card and the diagnostics rows cannot drift into painting the same state
 * differently.
 *
 * The second line is the one fact that makes the state actionable, and it is
 * different per state on purpose:
 *
 *   awake        when it last answered — is this reading fresh?
 *   asleep       when it next wakes — how long until a change lands?
 *   pending      that something is waiting — no time, because there is none.
 *   uncertain    that the tower does not know — the honest non-answer.
 *   unreachable  that it owed an answer and did not give one.
 *
 * A link rather than a button: it navigates, and a person should be able to
 * middle-click it or see where it goes in the status bar.
 */
export function DeviceChip({ reading }: { reading: DeviceReading }) {
  const state = deriveDeviceState(reading.input);

  let sub: string;
  switch (state.state) {
    case "awake":
      sub = reading.input.deviceLastSeenAt
        ? `replied ${relativeTime(reading.input.deviceLastSeenAt)}`
        : "answering";
      break;
    case "asleep":
      sub = reading.nextWakeLabel ? `wakes ~${reading.nextWakeLabel}` : "sleeping";
      break;
    case "pending":
      sub = "change waiting";
      break;
    case "uncertain":
      sub = "state unconfirmed";
      break;
    default:
      sub = reading.nextWakeLabel
        ? `missed wake ${reading.nextWakeLabel}`
        : "not answering";
      break;
  }

  return (
    <Link
      className="device-chip"
      href="/device"
      data-device-state={state.state}
      data-testid="device-chip"
      // The visible text is two fragments — "Awake" and "replied 1 min ago" —
      // which a screen reader would run together with no relationship between
      // them. The label states what the chip is for as well as what it says,
      // because on its own "Awake, replied 1 min ago" does not announce that
      // pressing it goes anywhere.
      aria-label={`Device: ${state.label}, ${sub}. Open the Device page.`}
    >
      <span className="chip-dot" aria-hidden="true" />
      <span className="chip-text">
        <span className="chip-title" data-testid="device-chip-state">
          {state.label}
        </span>
        <span className="chip-sub">{sub}</span>
      </span>
    </Link>
  );
}
