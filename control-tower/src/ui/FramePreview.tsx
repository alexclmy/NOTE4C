"use client";

import { useEffect, useMemo, useRef } from "react";
import { FrameBuffer, pack } from "@/core/frame";
import {
  FRAME_HEIGHT,
  FRAME_WIDTH,
  PREVIEW_RGB_TRIPLETS,
} from "@/core/palette";
import type { DashboardDoc } from "@/core/model";
import { renderDashboard, PANEL_TIMEZONE } from "@/core/render";
import type { DashboardSources } from "@/core/render/data";

/**
 * The designer preview.
 *
 * This runs the SAME renderer the server uses to pack bytes for the device,
 * compiled for the browser, and paints palette indices straight to a canvas
 * with nearest-neighbour scaling. What you see here is byte-identical to what
 * would ship, which is the only reason a preview is worth showing at all.
 */
export function FramePreview({
  doc,
  sources,
  now,
  scale = 1,
  fluid = false,
  className,
  testId,
  onPacked,
}: {
  doc: DashboardDoc;
  sources: DashboardSources;
  now: Date;
  scale?: number;
  /**
   * Let CSS size the canvas instead of the scale factor.
   *
   * The canvas is always 400 x 300 *drawing* pixels — that never varies, and
   * it is why the pixel comparisons in the browser QA can read this element
   * with `toDataURL()` wherever it appears. What varies is how many CSS
   * pixels those are painted across. A fixed scale is right in the designer,
   * where 1:1 and 2:1 are meaningful settings; it is wrong in a thumbnail
   * grid, where the tile's width is whatever the grid gave it and the scale
   * would have to be computed from a measurement to match.
   */
  fluid?: boolean;
  className?: string;
  testId?: string;
  onPacked?: (bytes: Uint8Array) => void;
}) {
  const canvasRef = useRef<HTMLCanvasElement>(null);

  const frame = useMemo<FrameBuffer | { error: string }>(() => {
    try {
      return renderDashboard(doc, sources, { now, timeZone: PANEL_TIMEZONE });
    } catch (error) {
      return {
        error: error instanceof Error ? error.message : "The frame could not be rendered",
      };
    }
  }, [doc, sources, now]);

  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas || "error" in frame) return;
    const context = canvas.getContext("2d");
    if (!context) return;

    const image = context.createImageData(FRAME_WIDTH, FRAME_HEIGHT);
    for (let i = 0; i < frame.pixels.length; i += 1) {
      const rgb =
        PREVIEW_RGB_TRIPLETS[frame.pixels[i] as number] ?? PREVIEW_RGB_TRIPLETS[1];
      const offset = i * 4;
      image.data[offset] = rgb?.[0] ?? 255;
      image.data[offset + 1] = rgb?.[1] ?? 255;
      image.data[offset + 2] = rgb?.[2] ?? 255;
      image.data[offset + 3] = 255;
    }
    context.putImageData(image, 0, 0);

    if (onPacked) {
      try {
        onPacked(pack(frame));
      } catch {
        // pack() throwing means a module wrote a non-palette index. The
        // preview still shows what happened; the push path refuses it.
      }
    }
  }, [frame, onPacked]);

  if ("error" in frame) {
    return (
      <div className="preview-error" role="alert" data-testid={testId}>
        {frame.error}
      </div>
    );
  }

  return (
    <canvas
      ref={canvasRef}
      width={FRAME_WIDTH}
      height={FRAME_HEIGHT}
      className={`frame-preview ${className ?? ""}`}
      data-testid={testId}
      style={{
        ...(fluid
          ? { width: "100%", height: "auto" }
          : { width: FRAME_WIDTH * scale, height: FRAME_HEIGHT * scale }),
        // Nearest neighbour: a smoothed e-paper preview would be a lie.
        imageRendering: "pixelated",
      }}
      aria-label="Dashboard preview at the exact device palette"
    />
  );
}
