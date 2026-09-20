"use client";

import { useEffect, useRef, useState, type ReactNode } from "react";

/** The panel, in device pixels. Not a layout choice: it is the hardware. */
const PANEL_W = 400;
const PANEL_H = 300;

/**
 * Show a 400 x 300 panel at whatever size the column it is in allows.
 *
 * A `transform: scale`, never a width. Everything inside this wrapper is laid
 * out at exactly 400 x 300 CSS pixels, because what goes inside is either the
 * renderer's own canvas — whose pixels the browser QA compares against fixed
 * expectations — or an `<img>` of a PNG the device produced. Scaling the
 * container instead would resample the panel at a size nothing else in the
 * product agrees on, and would make "the preview is the renderer" false.
 *
 * A transform leaves the element's layout box at full size, so a scaled-down
 * panel would still reserve 300 px of height and leave a gap under it, and a
 * scaled-up one would overflow its card. The wrapper therefore clips and is
 * given the scaled height explicitly, which is what `--panel-h` carries.
 *
 * Measured with a ResizeObserver rather than a media query: the same component
 * appears in a full-width card on Overview, in a two-thirds column in the
 * editor and in a 180 px thumbnail in the send dialog, and the width that
 * matters is the parent's, not the viewport's.
 */
export function ScaledPanel({
  children,
  min = 0.5,
  max = 1.45,
  /** The parent's own border and padding, which the scale must not count. */
  inset = 36,
  className,
  testId,
}: {
  children: ReactNode;
  min?: number;
  max?: number;
  inset?: number;
  className?: string;
  testId?: string;
}) {
  const boxRef = useRef<HTMLDivElement>(null);
  /**
   * Starts at `min`, not at 1.
   *
   * The first client render happens before anything has been measured. Coming
   * in at 1 meant a phone painted a 400 px panel inside a 343 px column for
   * one frame — long enough to scroll the document sideways, which is the one
   * thing the mobile QA measures for. Starting small and growing is invisible;
   * starting large and shrinking is a visible jump and a scrollbar.
   */
  const [scale, setScale] = useState(min);

  useEffect(() => {
    const node = boxRef.current;
    if (!node) return;
    const measure = (): void => {
      const usable = Math.max(1, node.clientWidth - inset);
      setScale(Math.max(min, Math.min(usable / PANEL_W, max)));
    };
    measure();
    const observer = new ResizeObserver(measure);
    observer.observe(node);
    return () => observer.disconnect();
  }, [min, max, inset]);

  return (
    <div ref={boxRef} className={className} data-testid={testId}>
      <div
        className="panel-scale-wrap"
        style={
          {
            // +2 for the panel's own 2 px outline, which sits outside its box.
            "--panel-h": `${Math.round(PANEL_H * scale + 2)}px`,
          } as React.CSSProperties
        }
      >
        <div
          className="panel-scale"
          style={{ "--panel-scale": scale } as React.CSSProperties}
        >
          {children}
        </div>
      </div>
    </div>
  );
}
