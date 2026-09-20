"use client";

import type { ReactNode } from "react";
import { Button } from "./components";

/**
 * Loading, failed and empty, said the same way on every page.
 *
 * Each page used to write its own: "Reading the device and the sources.",
 * "Loading the dashboard.", an error paragraph with no way to try again on
 * four pages out of six. The words were fine; the inconsistency meant a reader
 * had to work out, per page, whether they were looking at a slow read, a dead
 * tower or an empty store.
 *
 * The skeleton is deliberately plain — grey rules at the height of the cards
 * that are coming — because a shimmer is motion this product does not spend,
 * and because a skeleton that looks like content is a lie about what has been
 * read.
 */

export function PageLoading({
  label = "Reading.",
  rows = 3,
  testId = "page-loading",
}: {
  label?: string;
  rows?: number;
  testId?: string;
}) {
  return (
    <div className="page-state" data-testid={testId} role="status" aria-live="polite">
      <p className="empty">{label}</p>
      <div className="skeleton" aria-hidden="true">
        {Array.from({ length: rows }, (_, index) => (
          <div className="skeleton-card" key={index} />
        ))}
      </div>
    </div>
  );
}

export function PageError({
  message,
  onRetry,
  retryLabel = "Try again",
  testId = "page-error",
}: {
  message: string;
  onRetry?: () => void;
  retryLabel?: string;
  testId?: string;
}) {
  return (
    <div className="page-state" data-testid={testId}>
      <p className="error-note" role="alert" data-testid="error-note">
        {message}
      </p>
      {onRetry && (
        <Button onClick={onRetry} testId="page-retry">
          {retryLabel}
        </Button>
      )}
    </div>
  );
}

export function PageEmpty({
  children,
  action,
  testId = "page-empty",
}: {
  children: ReactNode;
  action?: ReactNode;
  testId?: string;
}) {
  return (
    <div className="page-state" data-testid={testId}>
      <p className="empty">{children}</p>
      {action}
    </div>
  );
}
