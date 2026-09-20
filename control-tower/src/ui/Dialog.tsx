"use client";

import { useCallback, useEffect, useRef, useState, type ReactNode } from "react";
import { Button } from "./components";
import { useExclusiveOverlay, useScrollLock } from "./overlay";

/**
 * The modal dialog, with the four things the previous one was missing.
 *
 * The dialogs in this product guard the actions that reach hardware — PUSH,
 * RESTART, SLEEP, REAL DEVICE — so "it looks like a dialog" is not enough. A
 * keyboard user could Tab straight out of the old one into the page behind and
 * keep operating it while it was supposedly modal, Escape did nothing, and
 * focus was never given back to whatever opened it. So:
 *
 *  - focus moves into the dialog on open, to the typed-confirmation field when
 *    there is one and to the dialog itself otherwise — by an explicit marker,
 *    not by DOM order, so a body that grows a link cannot steal it;
 *  - Tab and Shift+Tab cycle inside it and cannot leave;
 *  - Escape cancels, which is what every reader expects and what makes a
 *    dialog dismissable one-handed;
 *  - the focus that was there when it opened is restored when it closes, so a
 *    cancel returns the keyboard to the button that was pressed;
 *  - the page behind does not scroll.
 *
 * Escape cancels even for destructive dialogs on purpose: cancelling is the
 * safe direction, and the typed word is what stands between the operator and
 * the panel, not the difficulty of closing the window.
 */

const FOCUSABLE =
  'a[href], button:not([disabled]), input:not([disabled]), select:not([disabled]), textarea:not([disabled]), [tabindex]:not([tabindex="-1"])';

/**
 * The element that gets focus when a dialog opens, if it is present.
 *
 * An explicit marker rather than "the first focusable in the panel", and the
 * difference is not cosmetic. The typed-confirmation field is rendered *after*
 * the caller's body, so "first focusable" only lands on it as long as no
 * dialog body ever contains a link, a disclosure or a button. The moment one
 * does — and bodies here are prose written by whoever adds the dialog — focus
 * silently moves to that element instead, the operator types the confirmation
 * word into nothing, and the gate on every operation that reaches the panel
 * becomes a gate on noticing where the caret went. The marker makes the rule
 * the docblock states true by construction instead of true by coincidence.
 */
const AUTOFOCUS_MARKER = "data-dialog-autofocus";

export function Dialog({
  open,
  title,
  subtitle,
  children,
  wide = false,
  head,
  footer,
  onClose,
  labelledBy,
  testId,
  id = "dialog",
}: {
  open: boolean;
  title: string;
  /** A line under the title, for a dialog that is offering a choice. */
  subtitle?: ReactNode;
  children: ReactNode;
  /**
   * A roomier panel, for a dialog whose content is a grid rather than prose.
   *
   * 560 px is right for a sentence and two buttons and wrong for five
   * thumbnails: at the narrow width they reflow to two columns and the last
   * one sits alone on a third row.
   */
  wide?: boolean;
  /**
   * The close control, in the dialog's own header bar.
   *
   * Supplied by the caller rather than rendered here, because whether a dialog
   * may be closed at all is the caller's question: the send flow disables it
   * while a push is in flight, since closing would discard the outcome of an
   * operation that is already reaching the panel.
   */
  head?: ReactNode;
  /** The action row. `null` for a dialog whose actions are in its body. */
  footer: ReactNode;
  onClose: () => void;
  labelledBy?: string;
  testId?: string;
  /** Identity for the one-overlay-at-a-time rule. */
  id?: string;
}) {
  const panelRef = useRef<HTMLDivElement>(null);
  const restoreRef = useRef<HTMLElement | null>(null);

  useExclusiveOverlay(id, open);
  useScrollLock(open);

  // Remember the trigger before the dialog paints over it.
  useEffect(() => {
    if (!open) return;
    restoreRef.current = document.activeElement as HTMLElement | null;
    return () => {
      // Guard: the trigger may have been unmounted by the very action the
      // dialog confirmed, and focusing a detached node throws nothing but
      // silently drops focus to <body>.
      const target = restoreRef.current;
      if (target && document.contains(target)) target.focus();
    };
  }, [open]);

  // Move focus in, once, after the content exists.
  //
  // Two targets and no third: the marked field when there is one, the dialog
  // itself otherwise. Falling back to the panel rather than to its first
  // button is also deliberate — a dialog element with `role="dialog"` and
  // `aria-modal` is what a screen reader wants to be handed, and focusing
  // "Cancel" both skips the announcement and puts the keyboard on the one
  // control the operator is least likely to have opened the dialog for.
  useEffect(() => {
    if (!open) return;
    const panel = panelRef.current;
    if (!panel) return;
    const marked = panel.querySelector<HTMLElement>(`[${AUTOFOCUS_MARKER}]`);
    (marked ?? panel).focus();
  }, [open]);

  const onKeyDown = useCallback(
    (event: React.KeyboardEvent) => {
      if (event.key === "Escape") {
        event.stopPropagation();
        onClose();
        return;
      }
      if (event.key !== "Tab") return;
      const panel = panelRef.current;
      if (!panel) return;
      const focusable = [...panel.querySelectorAll<HTMLElement>(FOCUSABLE)].filter(
        (node) => node.offsetParent !== null || node === document.activeElement,
      );
      if (focusable.length === 0) {
        event.preventDefault();
        return;
      }
      const first = focusable[0] as HTMLElement;
      const last = focusable[focusable.length - 1] as HTMLElement;
      if (event.shiftKey && document.activeElement === first) {
        event.preventDefault();
        last.focus();
      } else if (!event.shiftKey && document.activeElement === last) {
        event.preventDefault();
        first.focus();
      }
    },
    [onClose],
  );

  if (!open) return null;

  return (
    <div
      className="modal-backdrop"
      // A click on the backdrop cancels, the same as Escape. Clicks inside the
      // panel stop here, so a drag that ends outside the panel does not close
      // a dialog somebody was typing a confirmation word into.
      onMouseDown={(event) => {
        if (event.target === event.currentTarget) onClose();
      }}
    >
      <div
        className={wide ? "modal modal-wide" : "modal"}
        role="dialog"
        aria-modal="true"
        aria-label={labelledBy ? undefined : title}
        aria-labelledby={labelledBy}
        ref={panelRef}
        tabIndex={-1}
        onKeyDown={onKeyDown}
        data-testid={testId}
      >
        <div className="modal-head">
          <div>
            <h2>{title}</h2>
            {subtitle && <p>{subtitle}</p>}
          </div>
          {head}
        </div>
        <div className="modal-content">{children}</div>
        {footer !== null && footer !== undefined && (
          <div className="modal-actions">{footer}</div>
        )}
      </div>
    </div>
  );
}

/**
 * Confirmation dialog. When confirmWord is given the operator has to type it
 * exactly, which is the gate on anything that reaches the physical device.
 *
 * Same props and same testids as the version this replaces, so the existing
 * browser QA keeps its grip; everything new is behaviour the old one lacked.
 */
export function ConfirmDialog({
  open,
  title,
  body,
  confirmWord,
  confirmLabel = "Confirm",
  onConfirm,
  onCancel,
  id,
}: {
  open: boolean;
  title: string;
  body: ReactNode;
  confirmWord?: string;
  confirmLabel?: string;
  onConfirm: () => void;
  onCancel: () => void;
  id?: string;
}) {
  const [typed, setTyped] = useState("");

  useEffect(() => {
    if (open) setTyped("");
  }, [open]);

  const ready = !confirmWord || typed === confirmWord;

  return (
    <Dialog
      open={open}
      title={title}
      onClose={onCancel}
      id={id ?? `confirm:${title}`}
      testId="confirm-dialog"
      head={
        <Button
          className="modal-close"
          onClick={onCancel}
          ariaLabel="Close"
          testId="confirm-close"
        >
          ✕
        </Button>
      }
      footer={
        <>
          <Button variant="quiet" onClick={onCancel} testId="confirm-cancel">
            Cancel
          </Button>
          <Button
            variant="danger"
            onClick={onConfirm}
            disabled={!ready}
            testId="confirm-ok"
          >
            {confirmLabel}
          </Button>
        </>
      }
    >
      <div className="modal-body">{body}</div>
      {confirmWord && (
        <label className="field">
          <span>
            Type <code>{confirmWord}</code> to continue
          </span>
          <input
            value={typed}
            onChange={(event) => setTyped(event.target.value)}
            data-testid="confirm-word"
            autoComplete="off"
            // See AUTOFOCUS_MARKER above: this is what makes "focus lands on
            // the typed confirmation" a property of the dialog rather than an
            // accident of where the field sits in the body.
            data-dialog-autofocus=""
          />
        </label>
      )}
    </Dialog>
  );
}
