"use client";

import { createContext, useCallback, useContext, useEffect, useState } from "react";

/**
 * Does the page inside the shell already show the device's state?
 *
 * The rail carries the state badge and the "Interactive 15 min" request on
 * every page, because on most pages it is the only place they exist. Overview
 * and Device also render a StateStrip, which says the same two things larger
 * and with the reason spelled out. On a wide screen those live in different
 * places — one in a column to the left, one at the top of the page — and
 * reading both costs nothing. On a phone the rail reflows into a horizontal
 * strip directly above the page, so the badge and the button appear twice,
 * stacked, thirty pixels apart. That is the duplicate this removes.
 *
 * The shell cannot know which routes carry a StateStrip by looking at the
 * pathname without keeping a second list that drifts the day a page gains or
 * loses one, and it cannot ask the DOM without `:has`, which would make a
 * layout rule depend on a selector whose support and cost are both harder to
 * reason about than a piece of state. So the StateStrip says so itself: it
 * registers while it is mounted, the shell counts the registrations, and the
 * answer is true exactly when something below is already showing it.
 */

type Register = () => () => void;

const PageDeviceStateContext = createContext<Register | null>(null);

/** Wrap the shell's children in this, with the `register` from the hook below. */
export const PageDeviceStateProvider = PageDeviceStateContext.Provider;

/**
 * For the shell: whether the page is showing the device state, and the
 * registration function to hand down to it.
 */
export function usePageDeviceStatePresence(): { present: boolean; register: Register } {
  const [count, setCount] = useState(0);

  // A count rather than a boolean, so a page with two strips — or the moment
  // during a route change when the next page has mounted and the previous one
  // has not yet cleaned up — cannot leave the shell believing there is one
  // when there is none.
  const register = useCallback<Register>(() => {
    setCount((n) => n + 1);
    return () => setCount((n) => n - 1);
  }, []);

  return { present: count > 0, register };
}

/** For a component that shows the device state: declare it to the shell. */
export function useRegisterPageDeviceState(): void {
  const register = useContext(PageDeviceStateContext);
  useEffect(() => {
    // Outside the shell — a test harness, a future standalone view — there is
    // no rail to deduplicate against and nothing to announce to.
    if (!register) return;
    return register();
  }, [register]);
}
