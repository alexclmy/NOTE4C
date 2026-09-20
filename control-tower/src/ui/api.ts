"use client";

import { useCallback, useEffect, useRef, useState } from "react";

/**
 * Client side API helper.
 *
 * Every mutation echoes the CSRF cookie in a header. The cookie is readable by
 * this page and unreadable by anyone else's, which is the whole mechanism.
 */

export const CSRF_COOKIE = "note4c_tower_csrf";

export function csrfToken(): string {
  const match = document.cookie
    .split(";")
    .map((part) => part.trim())
    .find((part) => part.startsWith(`${CSRF_COOKIE}=`));
  return match ? decodeURIComponent(match.slice(CSRF_COOKIE.length + 1)) : "";
}

export class ApiError extends Error {
  readonly status: number;
  readonly code: string;
  constructor(status: number, code: string, detail: string) {
    super(detail);
    this.name = "ApiError";
    this.status = status;
    this.code = code;
  }
}

async function parse<T>(response: Response): Promise<T> {
  const text = await response.text();
  let body: unknown = null;
  try {
    body = text.length > 0 ? JSON.parse(text) : null;
  } catch {
    body = null;
  }
  if (!response.ok) {
    const shaped = body as { error?: string; detail?: string } | null;
    throw new ApiError(
      response.status,
      shaped?.error ?? "request_failed",
      shaped?.detail ?? `Request failed with ${response.status}`,
    );
  }
  return body as T;
}

/**
 * A read, optionally abandonable.
 *
 * `signal` is not a nicety. A browser opens at most six connections to one
 * origin, and a device read that is waiting out a ten-second transport timeout
 * holds one of those for the whole of it. Two pages' worth of abandoned reads —
 * a status and a config per visit, and the visit before that — is the pool
 * gone, and from then on *every* request queues behind them, including the
 * local-file reads this page paints from. Measured in the browser suite: the
 * second crossing to `/device` during a stalled read never got its own answer,
 * because five sockets were still being held by pages nobody was looking at.
 *
 * So a component that gives up on a read says so, and the socket goes back.
 * The server-side read it started may still finish on its own — that is the
 * device's own timeout and not ours to cancel — but nothing is waiting on it.
 */
export async function apiGet<T>(
  path: string,
  options: { signal?: AbortSignal } = {},
): Promise<T> {
  return parse<T>(
    await fetch(path, {
      cache: "no-store",
      ...(options.signal ? { signal: options.signal } : {}),
    }),
  );
}

/** True for a read this page deliberately abandoned. Never an error to show. */
export function isAbort(caught: unknown): boolean {
  return caught instanceof DOMException && caught.name === "AbortError";
}

export async function apiSend<T>(
  path: string,
  method: "POST" | "PUT" | "PATCH" | "DELETE",
  body?: unknown,
): Promise<T> {
  return parse<T>(
    await fetch(path, {
      method,
      headers: {
        "content-type": "application/json",
        "x-csrf-token": csrfToken(),
      },
      body: body === undefined ? undefined : JSON.stringify(body),
      cache: "no-store",
    }),
  );
}

/** Relative time, with the absolute value carried in a title attribute. */
export function relativeTime(iso: string | null | undefined, now = Date.now()): string {
  if (!iso) return "never";
  const then = new Date(iso).getTime();
  if (Number.isNaN(then)) return "unknown";
  const seconds = Math.round((now - then) / 1000);
  if (seconds < 0) return "in the future";
  if (seconds < 10) return "just now";
  if (seconds < 60) return `${seconds} s ago`;
  const minutes = Math.round(seconds / 60);
  if (minutes < 60) return `${minutes} min ago`;
  const hours = Math.round(minutes / 60);
  if (hours < 24) return `${hours} h ago`;
  const days = Math.round(hours / 24);
  return `${days} d ago`;
}

export function absoluteTime(iso: string | null | undefined): string {
  if (!iso) return "never";
  const date = new Date(iso);
  if (Number.isNaN(date.getTime())) return "unknown";
  return date.toLocaleString("en-CA", { hour12: false });
}

// ------------------------------------------------------------- polling --

/**
 * A GET that keeps itself current, and admits when it has not.
 *
 * The defect this exists for: Overview read once on mount and never again. The
 * timestamps kept ageing on screen — "45 min ago" — while nothing re-read and
 * nothing said the picture was old. For a page whose subject is a device that
 * sleeps and wakes on its own, that is the display quietly diverging from the
 * thing it claims to describe.
 *
 * Three rules:
 *
 *  - **Only while the tab is visible.** A background tab polling a local
 *    server forever is rude to the laptop it is on, and nobody is reading it.
 *    Coming back to the tab triggers an immediate read, which is the moment
 *    the freshness actually matters.
 *  - **Only the tower, and never on the poll's own account.** This polls a
 *    tower route, and a poll is always sent WITHOUT `force=1`. On the Overview
 *    route that is the difference between reading a cached snapshot and making
 *    the tower open a socket at the panel, fetch a forecast and spawn a
 *    subprocess — every sixty seconds, for as long as a tab is open. The
 *    explicit `reload()` a person triggers does force, because a person asking
 *    "read again" is asking for exactly that. Nothing here may be pointed at a
 *    route that writes.
 *  - **Staleness is reported, not hidden.** When the last successful read is
 *    older than twice the interval — a suspended laptop, a tower that stopped
 *    answering — `stale` goes true and the page says so instead of showing an
 *    old number as if it were current.
 */

export interface PolledResource<T> {
  data: T | null;
  error: string;
  /** When the last successful read landed, by this browser's clock. */
  readAt: number | null;
  stale: boolean;
  loading: boolean;
  /**
   * A read a *person* asked for is in flight.
   *
   * Separate from `loading`, which is true for the sixty-second timer and for
   * the short follow-up below as well. A button disabled on `loading` blinks
   * inert every time a background poll runs, and — worse — says nothing about
   * why. This is the one a control should label itself from, because it is
   * the one the reader caused.
   */
  reloading: boolean;
  /**
   * Read again now, as a person asking. Sends `force=1`, so a route that keeps
   * a cache is told to skip it. This is the button, not the timer.
   */
  reload: () => Promise<void>;
}

/**
 * How many quick follow-ups a provisional answer is allowed before the page
 * goes back to the ordinary interval.
 *
 * Bounded on purpose, and not because the server is expected to misbehave.
 * `settling` asks the page to come back until the payload stops calling itself
 * provisional, and a route that never manages to finish its slow half — a
 * source adapter that throws every time, say — would otherwise be re-read once
 * a second for as long as the tab is open. Fifteen seconds of attention, then
 * the sixty-second poll, which will pick the answer up whenever it arrives.
 */
const SETTLE_ATTEMPT_LIMIT = 15;

/** Add `force=1` without assuming the path has no query string of its own. */
function forced(path: string): string {
  return `${path}${path.includes("?") ? "&" : "?"}force=1`;
}

export function usePolledResource<T>(
  path: string,
  intervalMs: number,
  options: {
    enabled?: boolean;
    /**
     * True while the payload says the server is still assembling part of it.
     *
     * The companion to a route that answers immediately with what it already
     * has and finishes the slow half behind the response. Without this the
     * page would show the provisional answer until the next sixty-second tick,
     * which for a device read that takes two seconds is fifty-eight seconds of
     * a reader looking at "not read yet" for no reason.
     */
    settling?: (data: T) => boolean;
    /** How soon to look again while `settling`. */
    settleMs?: number;
  } = {},
): PolledResource<T> {
  const enabled = options.enabled ?? true;
  const { settling, settleMs = 1_000 } = options;
  const [data, setData] = useState<T | null>(null);
  const [error, setError] = useState("");
  const [readAt, setReadAt] = useState<number | null>(null);
  const [loading, setLoading] = useState(true);
  const [reloadingState, setReloadingState] = useState(false);
  /**
   * The same fact as `reloadingState`, readable without re-arming an effect.
   *
   * The poll interval has to know whether a person's read is out, and putting
   * the state in that effect's dependency list would tear the timer down and
   * rebuild it on every press — quietly resetting the sixty-second clock each
   * time somebody used the button.
   */
  const reloading = useRef(false);
  const setReloading = useCallback((value: boolean) => {
    reloading.current = value;
    setReloadingState(value);
  }, []);
  const [now, setNow] = useState(() => Date.now());
  // Survives re-renders so a slow response cannot overwrite a newer one.
  const sequence = useRef(0);

  /**
   * The read in flight, so a superseded or abandoned one gives its socket back.
   *
   * The ticket below still decides which answer is allowed to win; this decides
   * which requests are allowed to keep occupying the browser's connection pool
   * while they do it. See the note on `apiGet`.
   */
  const inFlight = useRef<AbortController | null>(null);

  /**
   * Which read owns the `reloading` flag.
   *
   * Not simply "any forced read is finishing": a person's read and the
   * tab-focus read both force, and whichever finished first would otherwise
   * re-enable the button while the other was still out. The ticket cannot be
   * used for this either — an ordinary poll superseding a forced read would
   * leave the flag set for ever and the button disabled with it.
   */
  const reloadTicket = useRef(0);

  const load = useCallback(
    async (force: boolean) => {
      const ticket = (sequence.current += 1);
      inFlight.current?.abort();
      const controller = new AbortController();
      inFlight.current = controller;
      setLoading(true);
      if (force) {
        reloadTicket.current = ticket;
        setReloading(true);
      }
      try {
        const payload = await apiGet<T>(force ? forced(path) : path, {
          signal: controller.signal,
        });
        if (ticket !== sequence.current) return;
        setData(payload);
        setReadAt(Date.now());
        setError("");
      } catch (caught) {
        if (ticket !== sequence.current || isAbort(caught)) return;
        setError(
          caught instanceof ApiError ? caught.message : "The tower did not answer",
        );
      } finally {
        if (inFlight.current === controller) inFlight.current = null;
        if (ticket === sequence.current) setLoading(false);
        if (reloadTicket.current === ticket) setReloading(false);
      }
    },
    [path],
  );

  // Leaving the page abandons whatever it was reading.
  useEffect(() => () => inFlight.current?.abort(), []);

  // The person-initiated read. Forces, so a cache is skipped.
  const reload = useCallback(() => load(true), [load]);

  /*
   * The first read on mount does NOT force, and that is the loading fix.
   *
   * It used to, on the reasoning that arriving at a page is somebody asking
   * and that the first read has no cached snapshot to serve anyway. The second
   * half of that was true and is the part that cost: `force=1` on
   * `/api/overview` means skip the cache and *wait* for a fresh read, and a
   * fresh read is a socket at a panel that is usually not there. Measured
   * against a device that accepts a connection and never answers — a dropped
   * packet, a firewall, a `node` binary without macOS Local Network
   * permission — the mount read cost two serialised device timeouts before the
   * page could paint a single card, because the shell's own read is ahead of
   * it in the device mutex.
   *
   * So the mount asks for what the tower already knows, which comes back in
   * milliseconds, and the route starts the real read behind its own response.
   * `settling` below brings the answer back in about a second. The forcing
   * read still exists and is still what "Read again" sends, because that is a
   * person asking for exactly the round trip this one is avoiding.
   */
  useEffect(() => {
    if (!enabled) return;
    void load(false);
  }, [load, enabled]);

  /*
   * Come back quickly for the half the server is still assembling.
   *
   * The predicate is held in a ref and deliberately not in the dependency
   * list. Callers write it inline — `(data) => !data.observed` — which is a new
   * function on every render, and an effect that depended on it would clear
   * and re-arm its timer on every render and therefore never fire once. The
   * *value* it is asked about is `data`, which is in the list, so the timer is
   * still re-evaluated exactly when the answer could have changed.
   */
  const settlingRef = useRef(settling);
  settlingRef.current = settling;
  const settleAttempts = useRef(0);

  useEffect(() => {
    if (!enabled || data === null) return;
    if (settlingRef.current?.(data) !== true) {
      settleAttempts.current = 0;
      return;
    }
    if (settleAttempts.current >= SETTLE_ATTEMPT_LIMIT) return;
    const timer = setTimeout(() => {
      settleAttempts.current += 1;
      if (document.visibilityState !== "hidden") void load(false);
    }, settleMs);
    return () => clearTimeout(timer);
  }, [data, enabled, load, settleMs]);

  useEffect(() => {
    if (!enabled) return;

    const tick = () => {
      setNow(Date.now());
      // Never on top of a read somebody pressed for. `load` abandons whatever
      // it supersedes, and a timer cancelling a person's twenty-one-second
      // "Check now" a few seconds in — to answer it from a cache — is the
      // timer deciding the reader did not mean it.
      if (reloading.current) return;
      // Unforced. The timer is not a person asking.
      if (document.visibilityState === "visible") void load(false);
    };
    const timer = setInterval(tick, intervalMs);

    const onVisible = () => {
      if (document.visibilityState === "visible") {
        setNow(Date.now());
        // Coming back to the tab IS a person arriving, so this one forces.
        void load(true);
      }
    };
    document.addEventListener("visibilitychange", onVisible);

    return () => {
      clearInterval(timer);
      document.removeEventListener("visibilitychange", onVisible);
    };
  }, [load, intervalMs, enabled]);

  // A second, slower clock so "this reading is old" appears even when the poll
  // itself is what stopped — which is the case the banner is for.
  useEffect(() => {
    if (!enabled) return;
    const timer = setInterval(() => setNow(Date.now()), Math.min(intervalMs, 30_000));
    return () => clearInterval(timer);
  }, [intervalMs, enabled]);

  const stale = readAt !== null && now - readAt > intervalMs * 2;

  return { data, error, readAt, stale, loading, reloading: reloadingState, reload };
}
