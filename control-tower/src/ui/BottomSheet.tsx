"use client";

import { useEffect, type ReactNode } from "react";
import { useExclusiveOverlay } from "./overlay";

/**
 * The inspector, where a phone can reach it.
 *
 * In the designer the selected module's options used to sit about two screens
 * below the canvas in single-column layout: select a tile, scroll down, type,
 * scroll up to see what it did. This puts the same content in a sheet anchored
 * to the bottom of the viewport, at a little over half the height, so the
 * canvas stays visible above it and edits can be watched as they are made.
 *
 * Three deliberate choices:
 *
 *  - **It is not modal.** There is no dimming backdrop and no focus trap,
 *    because the thing you are editing is the thing behind it and you must be
 *    able to drag a tile with the sheet open. It is a `region`, not a
 *    `dialog`.
 *  - **It mounts its children once.** On a wide screen `inline` renders them
 *    in the sidebar and the sheet does not exist; on a narrow one the sidebar
 *    copy does not exist. One subtree either way, so nothing edits a stale
 *    copy and the DOM never holds two inspectors.
 *  - **It registers as an overlay**, so a confirmation dialog and this sheet
 *    can never be on screen together — the caller has to close one first, and
 *    in development it is told so loudly.
 */
export function BottomSheet({
  open,
  title,
  onClose,
  children,
  testId = "bottom-sheet",
  id = "sheet",
}: {
  open: boolean;
  title: ReactNode;
  onClose: () => void;
  children: ReactNode;
  testId?: string;
  id?: string;
}) {
  useExclusiveOverlay(id, open);

  // Escape closes, as it does for the dialog. Bound to the document because
  // the sheet does not hold focus: the point is that the canvas still does.
  useEffect(() => {
    if (!open) return;
    const onKey = (event: KeyboardEvent) => {
      if (event.key === "Escape") onClose();
    };
    document.addEventListener("keydown", onKey);
    return () => document.removeEventListener("keydown", onKey);
  }, [open, onClose]);

  if (!open) return null;

  return (
    <div
      className="sheet"
      role="region"
      aria-label={typeof title === "string" ? title : "Details"}
      data-testid={testId}
    >
      <div className="sheet-head">
        <span className="sheet-grip" aria-hidden="true" />
        <h2>{title}</h2>
        <button
          type="button"
          className="btn btn-quiet sheet-close"
          onClick={onClose}
          data-testid="sheet-close"
        >
          Close
        </button>
      </div>
      <div className="sheet-body">{children}</div>
    </div>
  );
}
