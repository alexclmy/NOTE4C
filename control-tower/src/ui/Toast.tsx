"use client";

import {
  createContext,
  useCallback,
  useContext,
  useEffect,
  useRef,
  useState,
  type ReactNode,
} from "react";

/**
 * One toast, bottom centre, for three and a bit seconds.
 *
 * What it is for: saying that something small and reversible just happened —
 * duplicated, archived, saved as v4, restored. Those actions used to produce
 * either nothing at all or a banner that stayed on the page until the next
 * navigation, and a banner is the wrong shape for "done, carry on".
 *
 * What it is NOT for, and this matters more: anything that touched the device.
 * A push has a dialog that shows every stage and its verdict, because "sent"
 * and "the device confirmed it is displaying this" are different facts and a
 * three-second message cannot carry that difference. Nothing in this product
 * may report a hardware outcome and then disappear.
 *
 * It is not an overlay. It does not register with src/ui/overlay.ts, does not
 * trap focus, and does not lock the page behind it: it can and does appear
 * while a dialog is open, which is the whole point of it being a report rather
 * than a modal. `role="status"` so it is announced without stealing focus.
 *
 * One at a time. A queue would mean a person waiting ten seconds to find out
 * what their last click did; the newest message replaces the previous one,
 * because the newest is the one they are still wondering about.
 */

const TOAST_MS = 3200;

const ToastContext = createContext<(message: string) => void>(() => {});

/** Show a short confirmation. Safe to call from anywhere under the provider. */
export function useToast(): (message: string) => void {
  return useContext(ToastContext);
}

export function ToastProvider({ children }: { children: ReactNode }) {
  const [message, setMessage] = useState<string>("");
  /**
   * A key that changes on every call, even when the words are identical.
   *
   * Pressing Duplicate twice produces the same sentence twice, and without a
   * changing key React reuses the element: the CSS animation does not re-run
   * and — worse — the live region's text never changes, so a screen reader
   * announces the first one and stays silent for the second. Remounting is
   * what makes a repeated action audible.
   */
  const [key, setKey] = useState(0);
  const timer = useRef<ReturnType<typeof setTimeout> | null>(null);

  const show = useCallback((next: string) => {
    if (timer.current !== null) clearTimeout(timer.current);
    setMessage(next);
    setKey((value) => value + 1);
    timer.current = setTimeout(() => setMessage(""), TOAST_MS);
  }, []);

  useEffect(() => {
    return () => {
      if (timer.current !== null) clearTimeout(timer.current);
    };
  }, []);

  return (
    <ToastContext.Provider value={show}>
      {children}
      {/*
        The region is always mounted and empty when there is nothing to say.
        A live region has to exist before its content changes for the change to
        be announced — mounting the region and its first sentence in the same
        commit is the classic way to write `role="status"` and have it stay
        silent.
      */}
      <div className="visually-hidden" role="status" data-testid="toast-live">
        {message}
      </div>
      {message !== "" && (
        <div className="toast" key={key} data-testid="toast" aria-hidden="true">
          {message}
        </div>
      )}
    </ToastContext.Provider>
  );
}
