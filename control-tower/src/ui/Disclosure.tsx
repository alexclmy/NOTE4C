"use client";

import { useId, useState, type ReactNode } from "react";

/**
 * The "i" that actually opens, on every input device.
 *
 * It replaces a span carrying `title` and `aria-label`. That span showed its
 * text to a mouse pointer that hovered for a second, and to a screen reader.
 * It showed nothing at all to a finger and nothing to a keyboard — and the
 * things it was hiding are not decoration: why a setting is on-device only,
 * what "always awake" costs in battery, what the tower does and does not do
 * with the hub token. On a phone, a measurable part of this product's
 * honesty was simply unreadable.
 *
 * So the note is a button that expands the text inline, below the row. It
 * pushes content down rather than floating over it, which on a narrow screen
 * is the only placement that can be read without covering what it explains.
 */
export function Disclosure({
  text,
  label = "Explain this",
  testId,
}: {
  text: ReactNode;
  /** What a screen reader announces for the button. */
  label?: string;
  testId?: string;
}) {
  const [open, setOpen] = useState(false);
  const id = useId();

  return (
    <span className="disclosure">
      <button
        type="button"
        className="disclosure-toggle"
        aria-expanded={open}
        aria-controls={id}
        aria-label={label}
        onClick={() => setOpen((value) => !value)}
        data-testid={testId}
      >
        i
      </button>
      {open && (
        <span className="disclosure-body" id={id} role="note" data-testid={testId ? `${testId}-body` : undefined}>
          {text}
        </span>
      )}
    </span>
  );
}

/**
 * A labelled fold for a block of secondary content.
 *
 * `<details>` rather than a hand-rolled toggle: it is keyboard operable,
 * announced correctly, and searchable by the browser's own find-on-page in
 * recent engines. The only thing added is the class hook and the testid.
 */
export function Fold({
  summary,
  children,
  defaultOpen = false,
  testId,
}: {
  summary: ReactNode;
  children: ReactNode;
  defaultOpen?: boolean;
  testId?: string;
}) {
  return (
    <details className="fold" open={defaultOpen} data-testid={testId}>
      <summary>{summary}</summary>
      <div className="fold-body">{children}</div>
    </details>
  );
}
