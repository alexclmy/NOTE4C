"use client";

import { useCallback, useEffect, useState } from "react";
import type { DeviceStateInput } from "@/core/power";
import { ApiError, apiGet, apiSend } from "./api";

/**
 * The shell's copy of "what is the device doing", without a second device read.
 *
 * The shell shows the state badge on every page, and the pages read the device
 * for their own reasons. Having both fetch would double the number of times
 * the tower knocks on a battery-powered panel's door for one page view, which
 * is exactly the kind of cost this product refuses to pay quietly.
 *
 * So there is one module-level reading, and whoever reads the device publishes
 * it here. The shell fetches once on mount, in case the page it is wrapping
 * never reads the device at all (Dashboards, Voice), and otherwise lives off
 * what the pages already know.
 *
 * Nothing here polls. Device contact is the scheduler's job; a browser tab
 * left open overnight must not become a source of traffic aimed at a device
 * that is trying to sleep.
 */

export interface DeviceReading {
  input: DeviceStateInput;
  /** False when this reading is the tower's record rather than a read. */
  observed: boolean;
  /**
   * The server's clock for when the device was read. The ordering key.
   *
   * Optional because a few callers build a reading by hand to hand to a dialog
   * or a chip, where there is nothing to order it against. `publishDeviceStatus`
   * treats a missing one as current, which is what those callers mean.
   */
  readAt?: string | null;
  nextWakeLabel: string | null;
  address: string | null;
  simulated: boolean;
}

/** The subset of GET /api/device/status this needs. Others carry more. */
export interface DeviceStatusLike {
  reachable: boolean;
  /**
   * Whether the device was actually asked. Absent means yes, which is what
   * every payload meant before a page could paint without asking.
   */
  observed?: boolean;
  simulated?: boolean;
  deviceMode?: "mock" | "real";
  deviceAddress?: string | null;
  power?: unknown;
  powerSupported?: boolean | null;
  powerIntent?: unknown;
  nextWake?: { at: string; estimated: boolean } | null;
  deviceLastSeenAt?: string | null;
  /** What the device last said, and when. See src/core/power.ts. */
  lastConfirmed?: unknown;
  /**
   * When the server took this reading. The ordering key below — not the time
   * the browser happened to parse the response.
   */
  readAt?: string;
}

let current: DeviceReading | null = null;
const listeners = new Set<(reading: DeviceReading | null) => void>();

function formatWake(iso: string, estimated: boolean): string {
  const date = new Date(iso);
  if (Number.isNaN(date.getTime())) return "at an unknown time";
  const when = date.toLocaleTimeString(undefined, {
    hour: "2-digit",
    minute: "2-digit",
  });
  return estimated ? `${when} (estimated)` : when;
}

/** Turn a status payload into the reading every page and the shell share. */
export function toDeviceReading(payload: DeviceStatusLike): DeviceReading {
  const observed = payload.observed ?? true;
  const input: DeviceStateInput = {
    reachable: payload.reachable,
    observed,
    power: (payload.power ?? null) as DeviceStateInput["power"],
    powerSupported: payload.powerSupported ?? null,
    intent: (payload.powerIntent ?? null) as DeviceStateInput["intent"],
    deviceMode: payload.deviceMode ?? (payload.simulated ? "mock" : "real"),
    nextWake: payload.nextWake ?? null,
    deviceLastSeenAt: payload.deviceLastSeenAt ?? null,
    lastConfirmed: (payload.lastConfirmed ??
      null) as DeviceStateInput["lastConfirmed"],
  };
  return {
    input,
    observed,
    readAt: payload.readAt ?? null,
    nextWakeLabel: payload.nextWake
      ? formatWake(payload.nextWake.at, payload.nextWake.estimated)
      : null,
    address: payload.deviceAddress ?? null,
    simulated: payload.simulated ?? payload.deviceMode === "mock",
  };
}

/**
 * Called by any page that has just read the device.
 *
 * Ordered by the server's own `readAt`, and that ordering is the fix for a
 * real race rather than a precaution. Three things publish here — the Device
 * page, Overview, and the shell's own mount read — and they run concurrently
 * against a device whose reads can take anything from one millisecond (a
 * routing refusal) to twenty seconds (a read that waits out a kernel
 * hold-down). Without an order, a slow poll started before a person pressed
 * "Check now" lands after it and puts the stale answer back on the badge.
 *
 * `readAt` is the server's clock for the moment the device was read, so it
 * compares like with like across pages. A payload that carries none is taken
 * as current, which is what the older callers meant.
 */
export function publishDeviceStatus(payload: DeviceStatusLike): boolean {
  const next = toDeviceReading(payload);
  /*
   * A reading nobody took never displaces one somebody did.
   *
   * The first paint of both pages now comes from the tower's own record with
   * no socket opened, and that payload has no observation time to order by —
   * there was no observation. Letting it through on the `readAt == null` path
   * below, which exists for callers that build a reading by hand, would let a
   * page arriving second put "not read yet" on a badge that a read had just
   * proved says Awake. That is the same stale-overwrites-fresh race the
   * ordering was added for, arriving from the other direction.
   */
  if (current?.observed === true && !next.observed) return false;
  if (current?.readAt != null && next.readAt != null) {
    if (Date.parse(next.readAt) < Date.parse(current.readAt)) return false;
  }
  current = next;
  for (const listener of listeners) listener(current);
  return true;
}

/** Test seam: forget the published reading between cases. */
export function resetDeviceReadingForTests(): void {
  current = null;
}

export interface DeviceStateHook extends Omit<Partial<DeviceReading>, "input"> {
  input: DeviceStateInput | null;
  busy: boolean;
  notice: string;
  refresh: () => Promise<void>;
  requestInteractive: (minutes?: number) => Promise<void>;
}

export function useDeviceState(): DeviceStateHook {
  const [reading, setReading] = useState<DeviceReading | null>(current);
  const [busy, setBusy] = useState(false);
  const [notice, setNotice] = useState("");

  useEffect(() => {
    listeners.add(setReading);
    return () => {
      listeners.delete(setReading);
    };
  }, []);

  const refresh = useCallback(async () => {
    try {
      publishDeviceStatus(await apiGet<DeviceStatusLike>("/api/device/status"));
    } catch {
      // A shell badge is not worth an error banner. The page the shell is
      // wrapping reports its own failures, with the context to explain them.
    }
  }, []);

  /*
   * The shell's own read, and why it is now two.
   *
   * The chip has to say something on every page, including the ones that never
   * read the device (Compositions, Voice), so the shell reads for itself when
   * no page has published. That read is a socket at the panel, and it is first
   * in the device mutex — so on a page that *does* read, the page's read used
   * to queue behind it and the reader paid both timeouts before anything
   * painted. That was half of the twenty seconds this is being corrected for.
   *
   * So the chip is filled immediately from what the tower already knows, which
   * opens nothing, and the real read follows behind it. Both publish through
   * the ordering above, so whichever is true of the later moment wins.
   */
  useEffect(() => {
    if (current !== null) return;
    void (async () => {
      try {
        publishDeviceStatus(
          await apiGet<DeviceStatusLike>("/api/device/status?observe=0"),
        );
      } catch {
        // Same reasoning as `refresh`: the chip is not worth a banner.
      }
      await refresh();
    })();
  }, [refresh]);

  /**
   * The one-click "I am about to touch the device" request.
   *
   * It posts the same intent the power panel does, and reports the same two
   * outcomes honestly: applied now, or held until the device next wakes. It
   * never says "done".
   */
  const requestInteractive = useCallback(
    async (minutes = 15) => {
      setBusy(true);
      setNotice("");
      try {
        const result = await apiSend<{
          applied?: boolean;
          pending?: boolean;
          detail?: string;
        }>("/api/device/power", "POST", {
          action: "set-mode",
          mode: "interactive",
          minutes,
        });
        setNotice(
          result.applied
            ? `Interactive for ${minutes} minutes: applied to the device.`
            : result.pending
              ? `Interactive for ${minutes} minutes: held until the device next wakes.`
              : (result.detail ?? "Request recorded."),
        );
        await refresh();
      } catch (caught) {
        setNotice(
          caught instanceof ApiError ? caught.message : "The tower did not answer",
        );
      } finally {
        setBusy(false);
      }
    },
    [refresh],
  );

  return {
    ...(reading ?? {}),
    input: reading?.input ?? null,
    busy,
    notice,
    refresh,
    requestInteractive,
  };
}
