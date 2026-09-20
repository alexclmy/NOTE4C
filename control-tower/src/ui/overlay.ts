"use client";

import { useEffect } from "react";

/**
 * One overlay at a time, enforced rather than hoped for.
 *
 * The rule this implements: a dialog and a bottom sheet are never mounted
 * together, and two dialogs are never mounted together. Two stacked overlays
 * on a 375 px screen produce a dialog whose backdrop covers a sheet whose
 * backdrop covers the page, three scroll containers, and a focus trap fighting
 * another focus trap.
 *
 * The registry is a module-level set because that is the only scope that
 * matches the thing being constrained — the document. React state would make
 * the check per-tree, which is exactly the case that breaks: the designer's
 * sheet and a confirmation dialog live in different subtrees.
 *
 * In development a violation throws, because the alternative is noticing it in
 * a screenshot three weeks later. In production it degrades to a console
 * warning: a mislayered overlay is ugly, not worth blanking the page for.
 */
const open = new Set<string>();

export function overlayCount(): number {
  return open.size;
}

/**
 * Register an overlay for as long as it is open.
 *
 * @param id    stable identity of this overlay, used in the message.
 * @param isOpen whether it is currently mounted and visible.
 */
export function useExclusiveOverlay(id: string, isOpen: boolean): void {
  useEffect(() => {
    if (!isOpen) return;
    if (open.size > 0) {
      const message = `Two overlays are open at once: ${[...open].join(", ")} and ${id}. Only one may be mounted; close the first before opening the second.`;
      if (process.env.NODE_ENV === "development") throw new Error(message);
      console.warn(message);
    }
    open.add(id);
    return () => {
      open.delete(id);
    };
  }, [id, isOpen]);
}

/**
 * Stop the page behind an overlay from scrolling.
 *
 * Kept beside the registry because both are properties of "an overlay is
 * open", and because the restore has to put back the exact previous value:
 * setting it to "" on close would clobber a body that was locked for another
 * reason.
 */
export function useScrollLock(active: boolean): void {
  useEffect(() => {
    if (!active) return;
    const previous = document.body.style.overflow;
    document.body.style.overflow = "hidden";
    return () => {
      document.body.style.overflow = previous;
    };
  }, [active]);
}
