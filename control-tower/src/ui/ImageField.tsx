"use client";

import { useCallback, useEffect, useRef, useState } from "react";
import { PREVIEW_RGB_TRIPLETS } from "@/core/palette";
import {
  ditherImage,
  encodeTile,
  decodeTile,
  type DitherOptions,
} from "@/core/render/imageDither";
import {
  IMAGE_FITS,
  IMAGE_MODES,
  type ImageFit,
  type ImageMode,
  type ImageSource,
} from "@/core/render/modules/image";

/**
 * The upload-and-dither widget for the Image module.
 *
 * Everything happens here, in the browser: the file is read, drawn onto a
 * canvas at the module's own pixel size, dithered to the four-colour palette,
 * and the finished tile is handed back through `onChange`. The original bytes
 * are held only in a ref for as long as this widget is mounted, so nudging a
 * knob can re-dither them; they are never stored. What is stored is the tile.
 *
 * The preview shows exactly the stored indices, painted in the panel's preview
 * colours, so what the owner sees is what the panel will show — pick and see,
 * nothing technical.
 */

const FIT_LABEL: Record<ImageFit, string> = {
  cover: "Fill",
  contain: "Fit",
};

const MODE_LABEL: Record<ImageMode, string> = {
  photo: "Photo",
  poster: "Poster",
};

/** Draw a loaded image onto a tile-sized canvas, cover or contain, on white. */
function drawToTile(
  image: CanvasImageSource,
  iw: number,
  ih: number,
  tileW: number,
  tileH: number,
  fit: ImageFit,
): ImageData | null {
  const canvas = document.createElement("canvas");
  canvas.width = tileW;
  canvas.height = tileH;
  const ctx = canvas.getContext("2d", { willReadFrequently: true });
  if (!ctx) return null;
  ctx.imageSmoothingEnabled = true;
  ctx.imageSmoothingQuality = "high";
  // White ground, so a "Fit" image's margins are paper, not black.
  ctx.fillStyle = "#ffffff";
  ctx.fillRect(0, 0, tileW, tileH);

  const scale =
    fit === "cover"
      ? Math.max(tileW / iw, tileH / ih)
      : Math.min(tileW / iw, tileH / ih);
  const dw = iw * scale;
  const dh = ih * scale;
  const dx = (tileW - dw) / 2;
  const dy = (tileH - dh) / 2;
  ctx.drawImage(image, dx, dy, dw, dh);
  return ctx.getImageData(0, 0, tileW, tileH);
}

/** Paint decoded palette indices into a canvas for the on-screen preview. */
function paintPreview(
  canvas: HTMLCanvasElement | null,
  data: string,
  tileW: number,
  tileH: number,
): void {
  if (!canvas || tileW <= 0 || tileH <= 0) return;
  canvas.width = tileW;
  canvas.height = tileH;
  const ctx = canvas.getContext("2d");
  if (!ctx) return;
  const indices = decodeTile(data, tileW, tileH);
  const out = ctx.createImageData(tileW, tileH);
  for (let i = 0; i < tileW * tileH; i += 1) {
    const [r, g, b] = PREVIEW_RGB_TRIPLETS[indices[i] as number] as readonly [
      number,
      number,
      number,
    ];
    out.data[i * 4] = r;
    out.data[i * 4 + 1] = g;
    out.data[i * 4 + 2] = b;
    out.data[i * 4 + 3] = 255;
  }
  ctx.putImageData(out, 0, 0);
}

export function ImageField({
  value,
  tileW,
  tileH,
  onChange,
}: {
  value: ImageSource;
  /** The module's pixel tile: what the picture is dithered to. */
  tileW: number;
  tileH: number;
  onChange: (next: ImageSource) => void;
}) {
  const sourceRef = useRef<{ image: CanvasImageSource; w: number; h: number } | null>(
    null,
  );
  const previewRef = useRef<HTMLCanvasElement | null>(null);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);

  // Keep the latest options in a ref so the resize effect re-dithers with them.
  const valueRef = useRef(value);
  valueRef.current = value;

  const dither = useCallback(
    (source: { image: CanvasImageSource; w: number; h: number }, next: ImageSource) => {
      if (tileW <= 0 || tileH <= 0) return;
      const imageData = drawToTile(source.image, source.w, source.h, tileW, tileH, next.fit);
      if (!imageData) {
        setError("This browser could not read the image.");
        return;
      }
      const opts: DitherOptions = {
        mode: next.mode,
        colourAmount: next.colourAmount,
        contrast: next.contrast,
      };
      const indices = ditherImage(imageData.data, tileW, tileH, opts);
      onChange({
        ...next,
        tileW,
        tileH,
        data: encodeTile(indices, tileW, tileH),
      });
    },
    [tileW, tileH, onChange],
  );

  const onFile = useCallback(
    (file: File | undefined) => {
      if (!file) return;
      setError(null);
      setBusy(true);
      const reader = new FileReader();
      reader.onload = () => {
        const img = new Image();
        img.onload = () => {
          sourceRef.current = { image: img, w: img.naturalWidth, h: img.naturalHeight };
          dither(sourceRef.current, valueRef.current);
          setBusy(false);
        };
        img.onerror = () => {
          setError("That file is not an image this browser can open.");
          setBusy(false);
        };
        img.src = String(reader.result);
      };
      reader.onerror = () => {
        setError("The file could not be read.");
        setBusy(false);
      };
      reader.readAsDataURL(file);
    },
    [dither],
  );

  // Re-dither when the module is resized while the source is still in memory.
  useEffect(() => {
    if (sourceRef.current) dither(sourceRef.current, valueRef.current);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [tileW, tileH]);

  // Repaint the preview whenever the stored tile changes.
  useEffect(() => {
    if (value.data && value.tileW > 0 && value.tileH > 0) {
      paintPreview(previewRef.current, value.data, value.tileW, value.tileH);
    }
  }, [value.data, value.tileW, value.tileH]);

  function setKnob(patch: Partial<ImageSource>): void {
    const next = { ...value, ...patch };
    if (sourceRef.current) {
      dither(sourceRef.current, next);
    } else {
      // No source in memory (a reopened session): remember the choice; it
      // takes effect on the next upload.
      onChange(next);
    }
  }

  const hasImage = Boolean(value.data) && value.tileW > 0 && value.tileH > 0;
  const canReprocess = sourceRef.current !== null;

  return (
    <div className="image-field" data-testid="opt-source">
      <div className="image-field-head">
        <label className="image-upload" data-testid="opt-source-upload-label">
          <input
            type="file"
            accept="image/*"
            data-testid="opt-source-upload"
            onChange={(event) => onFile(event.target.files?.[0])}
          />
          <span>{hasImage ? "Replace image" : "Upload an image"}</span>
        </label>
        {hasImage && (
          <button
            type="button"
            className="image-clear"
            data-testid="opt-source-clear"
            onClick={() => {
              sourceRef.current = null;
              onChange({ ...value, data: null, tileW: 0, tileH: 0 });
            }}
          >
            Remove
          </button>
        )}
      </div>

      {busy && <p className="field-hint" data-testid="opt-source-busy">Working…</p>}
      {error && (
        <p className="error-note" data-testid="opt-source-error">
          {error}
        </p>
      )}

      {hasImage && (
        <canvas
          ref={previewRef}
          className="image-preview"
          data-testid="opt-source-preview"
        />
      )}

      <div className="image-knobs">
        <div className="image-toggle" role="group" aria-label="Fit">
          {IMAGE_FITS.map((fit) => (
            <button
              type="button"
              key={fit}
              className="image-toggle-btn"
              aria-pressed={value.fit === fit}
              data-testid={`opt-source-fit-${fit}`}
              onClick={() => setKnob({ fit })}
            >
              {FIT_LABEL[fit]}
            </button>
          ))}
        </div>

        <div className="image-toggle" role="group" aria-label="Style">
          {IMAGE_MODES.map((mode) => (
            <button
              type="button"
              key={mode}
              className="image-toggle-btn"
              aria-pressed={value.mode === mode}
              data-testid={`opt-source-mode-${mode}`}
              onClick={() => setKnob({ mode })}
            >
              {MODE_LABEL[mode]}
            </button>
          ))}
        </div>

        <label className="image-slider">
          <span>Colour</span>
          <input
            type="range"
            min={0}
            max={100}
            step={5}
            value={value.colourAmount}
            data-testid="opt-source-colourAmount"
            onChange={(event) => setKnob({ colourAmount: Number(event.target.value) })}
          />
        </label>

        <label className="image-slider">
          <span>Contrast</span>
          <input
            type="range"
            min={-100}
            max={100}
            step={5}
            value={value.contrast}
            data-testid="opt-source-contrast"
            onChange={(event) => setKnob({ contrast: Number(event.target.value) })}
          />
        </label>
      </div>

      {hasImage && !canReprocess && (
        <p className="field-hint" data-testid="opt-source-reupload">
          Re-upload the image to change how it is dithered.
        </p>
      )}
    </div>
  );
}
