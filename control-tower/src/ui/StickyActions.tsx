"use client";

import type { ReactNode } from "react";

/**
 * The bar that follows unsaved work down the page.
 *
 * The defect it fixes, exactly: on the Device page the Apply button lived in
 * its own card *after* five cards of settings. A field you had just edited was
 * marked dirty in yellow and the control that would write it was two screens
 * away, with the count of pending changes invisible while you were making
 * them. On a phone that is the difference between "I changed three things" and
 * "I changed three things and only found out how many when I scrolled to the
 * bottom".
 *
 * So: when there is uncommitted work, a bar pins itself to the bottom of the
 * viewport with the count, the primary action and the way out. It clears the
 * mobile tab bar and the iOS home indicator, and it is a landmark with a
 * label, not a floating div, so it can be reached directly.
 *
 * It renders nothing when there is nothing pending. A bar that is always there
 * is chrome; a bar that appears when you create work is a consequence.
 */
export function StickyActions({
  visible,
  summary,
  children,
  detail,
  testId = "sticky-actions",
}: {
  visible: boolean;
  /** "3 changes waiting" — the count is the point. */
  summary: ReactNode;
  /** Buttons, primary last so it sits under the thumb on a phone. */
  children: ReactNode;
  /** Optional second line: a typed confirmation, or an explanation. */
  detail?: ReactNode;
  testId?: string;
}) {
  if (!visible) return null;
  return (
    <div
      className="sticky-actions"
      role="region"
      aria-label="Pending changes"
      data-testid={testId}
    >
      <div className="sticky-actions-inner">
        <span className="sticky-summary">{summary}</span>
        <span className="sticky-buttons">{children}</span>
      </div>
      {detail && <div className="sticky-detail">{detail}</div>}
    </div>
  );
}
