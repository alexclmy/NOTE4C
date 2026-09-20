"use client";

import { useEffect, useId, useState, type ReactNode } from "react";
import { absoluteTime, relativeTime } from "./api";

/**
 * Shared chrome. Copy rules from the plan: sentence case everywhere except
 * badges, verbs on buttons, and the state words are reserved and exact.
 */

export type BadgeKind =
  | "displayed"
  | "stored"
  | "selected"
  | "draft"
  | "pending"
  | "uncertain"
  | "failed"
  | "deduped"
  | "unreachable"
  | "simulated"
  | "archived"
  | "neutral"
  /** The device state vocabulary. See deriveDeviceState in src/core/power.ts. */
  | "awake"
  | "asleep";

const BADGE_TEXT: Record<BadgeKind, string> = {
  displayed: "DISPLAYED",
  stored: "STORED",
  selected: "SELECTED",
  draft: "draft",
  pending: "PENDING",
  uncertain: "UNCERTAIN",
  failed: "FAILED",
  deduped: "DEDUPED",
  unreachable: "UNREACHABLE",
  simulated: "SIMULATED",
  archived: "archived",
  neutral: "",
  awake: "AWAKE",
  asleep: "ASLEEP",
};

export function Badge({
  kind,
  children,
  title,
  testId,
}: {
  kind: BadgeKind;
  children?: ReactNode;
  title?: string;
  testId?: string;
}) {
  return (
    <span
      className={`badge badge-${kind}`}
      title={title}
      data-badge={kind}
      data-testid={testId}
    >
      {children ?? BADGE_TEXT[kind]}
    </span>
  );
}

/**
 * A bordered surface with an optional head.
 *
 * Four variants, and they differ by how much the card claims:
 *
 *   default  paper, 2 px ink, a 4 px shadow, a rule under the head.
 *   large    the same with a 5 px shadow — the subject of its column.
 *   plain    no rule under the head, so a title and a mono fact read as one
 *            line rather than as a header over a table. Most of the prototype's
 *            cards are this shape.
 *   quiet    dashed and unshadowed, on the canvas rather than on paper: for
 *            things that are not part of the daily loop (diagnostics links) or
 *            not finished (the voice experiment). A dashed border is this
 *            product's one way of saying "provisional" without colour.
 */
export function Card({
  title,
  actions,
  meta,
  variant = "default",
  children,
  id,
  testId,
}: {
  title?: string;
  actions?: ReactNode;
  /** A mono fact on the title's baseline: a battery line, a count, a stamp. */
  meta?: ReactNode;
  variant?: "default" | "large" | "plain" | "quiet";
  children: ReactNode;
  id?: string;
  testId?: string;
}) {
  const className = [
    "card",
    variant === "large" ? "card-lg" : "",
    variant === "quiet" ? "card-quiet" : "",
    variant === "plain" || variant === "quiet" ? "card-plain" : "",
  ]
    .filter(Boolean)
    .join(" ");

  return (
    <section className={className} id={id} data-testid={testId}>
      {(title || actions || meta) && (
        <header className="card-head">
          {title && <h2>{title}</h2>}
          {meta && <span className="mono-meta">{meta}</span>}
          {actions && <div className="card-actions">{actions}</div>}
        </header>
      )}
      <div className="card-body">{children}</div>
    </section>
  );
}

/**
 * A spaced mono capital label over the thing it names.
 *
 * WHAT WE KNOW, INK, WAKES EVERY, ADD A BLOCK. These are the only text in the
 * product below 12 px, and the rule that makes that acceptable is that they
 * are never prose and never alone: the label always sits directly above or
 * beside the content it names, so its job is to be recognised rather than read.
 */
export function MonoLabel({
  children,
  htmlFor,
}: {
  children: ReactNode;
  htmlFor?: string;
}) {
  if (htmlFor) {
    return (
      <label className="mono-label" htmlFor={htmlFor}>
        {children}
      </label>
    );
  }
  return <span className="mono-label">{children}</span>;
}

/**
 * "Technical details", folded away.
 *
 * A native `<details>` rather than a disclosure button: the content is prose
 * to read rather than a control to reach, it is never the thing a person came
 * for, and `<details>` is keyboard-operable, findable by the browser's own
 * in-page search, and printable open without any of it being written here.
 *
 * It is how this interface keeps its promise to be honest without being
 * technical at the top level: the digest, the entity id, the revision number
 * and the wire names are all present on the page, one press away, rather than
 * either shouted or omitted.
 */
export function TechDetails({
  children,
  summary = "Technical details",
  testId,
}: {
  children: ReactNode;
  summary?: string;
  testId?: string;
}) {
  return (
    <details className="tech-fold" data-testid={testId}>
      <summary>{summary}</summary>
      <div className="tech-body">{children}</div>
    </details>
  );
}

/**
 * A labelled value, with its explanation one tap away.
 *
 * The hint used to be a `title` attribute on the label: invisible to a finger,
 * invisible to a keyboard, and carrying things like why a setting cannot be
 * written remotely. Now the label itself is the control that reveals it — the
 * whole label, not a 14 px "i" beside it, because the label is already the
 * right size for a thumb and because a row of "i" glyphs down a settings page
 * is noise the reader has to learn to ignore.
 *
 * A row with no hint stays a plain span: nothing to press, nothing announced.
 */
export function Row({
  label,
  children,
  hint,
  testId,
}: {
  label: string;
  children: ReactNode;
  hint?: ReactNode;
  testId?: string;
}) {
  const [open, setOpen] = useState(false);
  const id = useId();

  return (
    <div className="row" data-testid={testId}>
      <div className="row-main">
        {hint ? (
          <button
            type="button"
            className="row-label row-label-hint"
            aria-expanded={open}
            aria-controls={id}
            onClick={() => setOpen((value) => !value)}
          >
            {label}
          </button>
        ) : (
          <span className="row-label">{label}</span>
        )}
        <span className="row-value">{children}</span>
      </div>
      {hint && open && (
        <p className="row-hint" id={id} data-testid="row-hint">
          {hint}
        </p>
      )}
    </div>
  );
}

export function Button({
  children,
  onClick,
  variant = "default",
  disabled,
  type = "button",
  title,
  testId,
  ariaPressed,
  ariaLabel,
  className,
  autoFocusInDialog,
}: {
  children: ReactNode;
  onClick?: () => void;
  /**
   * `primary` is the yellow raised one — at most one per view. `dark` is the
   * inverted one, used where a confirmation has to be visibly the heavier of
   * two choices without being destructive. `danger` is red and is only ever
   * a thing that cannot be undone.
   */
  variant?: "default" | "primary" | "danger" | "quiet" | "dark";
  disabled?: boolean;
  type?: "button" | "submit";
  title?: string;
  testId?: string;
  /** For segmented controls, where a button is a choice rather than a verb. */
  ariaPressed?: boolean;
  ariaLabel?: string;
  /** Layout only: `btn-grow`, `btn-cta`, `btn-on-panel`. Never colour. */
  className?: string;
  /** See AUTOFOCUS_MARKER in Dialog.tsx. */
  autoFocusInDialog?: boolean;
}) {
  return (
    <button
      type={type}
      className={`btn btn-${variant}${className ? ` ${className}` : ""}`}
      onClick={onClick}
      disabled={disabled}
      title={title}
      aria-pressed={ariaPressed}
      aria-label={ariaLabel}
      data-testid={testId}
      {...(autoFocusInDialog ? { "data-dialog-autofocus": "" } : {})}
    >
      {children}
    </button>
  );
}

/**
 * A full-width call to action, with its glyph pushed to the far edge.
 *
 * The glyph is text — an arrow, a plus — rather than an icon font or an SVG
 * set. This product ships no icon library, and the three glyphs it actually
 * needs are characters that every system font has. They are `aria-hidden`
 * because the label already says what the button does; "Show a composition
 * right arrow" is not a better announcement than "Show a composition".
 */
export function Cta({
  children,
  glyph,
  onClick,
  variant = "primary",
  disabled,
  testId,
}: {
  children: ReactNode;
  glyph: string;
  onClick?: () => void;
  variant?: "primary" | "default";
  disabled?: boolean;
  testId?: string;
}) {
  return (
    <button
      type="button"
      className={`btn btn-${variant} btn-cta${variant === "default" ? " btn-secondary" : ""}`}
      onClick={onClick}
      disabled={disabled}
      data-testid={testId}
    >
      <span>{children}</span>
      <span aria-hidden="true">{glyph}</span>
    </button>
  );
}

/** Relative on the face, absolute on hover. Applied to every timestamp. */
export function FreshnessStamp({ iso }: { iso: string | null | undefined }) {
  const [, setTick] = useState(0);
  useEffect(() => {
    const timer = setInterval(() => setTick((n) => n + 1), 30_000);
    return () => clearInterval(timer);
  }, []);
  return (
    <time className="num" title={absoluteTime(iso)} dateTime={iso ?? undefined}>
      {relativeTime(iso)}
    </time>
  );
}

export function Banner({
  tone,
  children,
  testId,
}: {
  tone: "attention" | "pending" | "info";
  children: ReactNode;
  testId?: string;
}) {
  return (
    <div className={`banner banner-${tone}`} role="status" data-testid={testId}>
      {children}
    </div>
  );
}

export function Empty({ children }: { children: ReactNode }) {
  return <p className="empty">{children}</p>;
}

export function ErrorNote({ children }: { children: ReactNode }) {
  if (!children) return null;
  return (
    <p className="error-note" role="alert" data-testid="error-note">
      {children}
    </p>
  );
}
