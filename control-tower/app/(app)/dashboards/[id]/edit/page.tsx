"use client";

import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { flushSync } from "react-dom";
import Link from "next/link";
import { useParams } from "next/navigation";
import { ApiError, apiGet, apiSend, relativeTime } from "@/ui/api";
import {
  Badge,
  Banner,
  Button,
  Card,
  Empty,
  ErrorNote,
} from "@/ui/components";
import { BottomSheet } from "@/ui/BottomSheet";
import { ComponentGallery } from "@/ui/ComponentGallery";
import { ConfirmDialog } from "@/ui/Dialog";
import { PageError, PageLoading } from "@/ui/PageState";
import { SendFlowDialog } from "@/ui/SendFlowDialog";
import { StickyActions } from "@/ui/StickyActions";
import { FramePreview } from "@/ui/FramePreview";
import { ModuleInspector } from "@/ui/ModuleInspector";
import { ThemePanel } from "@/ui/ThemePanel";
import { useToast } from "@/ui/Toast";
import { useDeviceState } from "@/ui/useDeviceState";
import { useMediaQuery, SINGLE_COLUMN_QUERY } from "@/ui/useMediaQuery";
import "@/ui/designer.css";
import {
  applyDrag,
  findFreeSlot,
  layoutProblems,
  newModuleId,
  type DashboardDoc,
  type ModuleInstance,
} from "@/core/model";
import { moduleDefinition } from "@/core/render/modules";
import { emptySources, type DashboardSources } from "@/core/render/data";
import {
  PANEL_TIMEZONE,
  RenderReport,
  renderDashboard,
  type DashboardContrast,
  type DashboardNote,
  type DashboardOverflow,
} from "@/core/render";
import { cellsToPixels, contentRect } from "@/core/render/types";
import { DEFAULT_THEME } from "@/core/theme";

interface VersionMeta {
  version: number;
  savedAt: string;
  note: string;
  rolledBackFrom: number | null;
}

interface RecordPayload {
  record: { doc: DashboardDoc; latestVersion: number };
  versions: VersionMeta[];
}

/** 2x on desktop so a 50 px cell is a comfortable 100 px drag target. */
const DESKTOP_ZOOM = 2;

/** The panel, in pixels. The stage is always this multiplied by the zoom. */
const PANEL_W = 400;

/**
 * How the stage is sized.
 *
 * "fit" is a continuous scale computed from the column the stage is in, and it
 * is the default on anything narrower than the two-column layout. It replaces
 * the old behaviour — force zoom to 1, then let a 400 px canvas overflow a
 * 375 px viewport and scroll sideways *while a drag was in progress*, with the
 * stage's `touch-action: none` and the wrapper's scrolling fighting over the
 * same finger.
 */
type ZoomMode = "fit" | 1 | 2;

export default function DesignerPage() {
  const params = useParams<{ id: string }>();
  const id = params.id;
  const toast = useToast();
  const device = useDeviceState();

  const [doc, setDoc] = useState<DashboardDoc | null>(null);
  const [saved, setSaved] = useState<string>("");
  const [versions, setVersions] = useState<VersionMeta[]>([]);
  const [latestVersion, setLatestVersion] = useState(0);
  const [selectedId, setSelectedId] = useState<string | null>(null);
  const [sources, setSources] = useState<DashboardSources>(() => emptySources());
  const [error, setError] = useState("");
  const [busy, setBusy] = useState(false);
  // Starts fitted, on every form factor. The first client render happens
  // before any media query has been answered, and starting at 2x meant a
  // phone painted an 800 px stage into a 375 px viewport for one frame — long
  // enough to scroll the page sideways.
  const [zoomMode, setZoomMode] = useState<ZoomMode>("fit");
  const [fitScale, setFitScale] = useState(1);
  const [now, setNow] = useState(() => new Date());
  const [announcement, setAnnouncement] = useState("");
  const [confirmDelete, setConfirmDelete] = useState<string | null>(null);
  const [versionsOpen, setVersionsOpen] = useState(false);
  const [sendOpen, setSendOpen] = useState(false);

  const singleColumn = useMediaQuery(SINGLE_COLUMN_QUERY);

  const stageRef = useRef<HTMLDivElement>(null);
  const columnRef = useRef<HTMLDivElement>(null);
  const versionsRef = useRef<HTMLDivElement>(null);
  const dragRef = useRef<{
    moduleId: string;
    mode: "move" | "resize";
    startX: number;
    startY: number;
    origin: ModuleInstance;
  } | null>(null);

  /*
   * Fit to width is the default at every size now, not just on a phone.
   *
   * It used to be 2x on a desktop, which worked only because the canvas column
   * was `1fr` beside a fixed 300 px sidebar and 800 px happened to be exactly
   * what was left at 1440. The column is now proportional, so 2x overflowed it
   * and the stage was painted across the inspector. Fitting is also what the
   * canvas should do: the panel is 400 x 300 whatever the window is, and the
   * useful question is "how much of my screen can I see it at", not "which of
   * two magnifications". 1:1 and 2x stay on the zoom cycle for close work, and
   * the wrapper scrolls when one of them does not fit.
   */
  useEffect(() => {
    setZoomMode("fit");
  }, [singleColumn]);

  /*
   * The fit scale follows the wrapper's own content box.
   *
   * Measured from a zero-height sentinel *inside* the card rather than from
   * the column outside it, because the number that matters is the width the
   * stage may occupy — which is the column minus the card's border and its
   * padding, and that padding changes with the density setting. Subtracting a
   * guessed constant was how a 375 px screen ended up four pixels wide of its
   * own viewport.
   */
  useEffect(() => {
    const sentinel = columnRef.current;
    if (!sentinel) return;
    const measure = () => {
      // 4 px for the stage's own 2 px outline, which is drawn outside its box.
      const usable = Math.max(160, sentinel.clientWidth - 4);
      setFitScale(Math.min(2, usable / PANEL_W));
    };
    measure();
    const observer = new ResizeObserver(measure);
    observer.observe(sentinel);
    return () => observer.disconnect();
  }, [doc !== null]);

  const zoom = zoomMode === "fit" ? fitScale : zoomMode;

  /*
   * The versions popover closes on an outside press and on Escape.
   *
   * A popover rather than a Dialog, deliberately. `src/ui/overlay.ts` allows
   * exactly one overlay at a time and throws in development if a second
   * mounts, and this list has to be openable from a page that can also have
   * the delete confirmation, the send flow or the inspector sheet up. It is
   * also not modal in any real sense: it is a menu anchored to the line that
   * describes it, and losing focus is the right way to dismiss one.
   */
  useEffect(() => {
    if (!versionsOpen) return;
    const onDown = (event: MouseEvent): void => {
      if (!versionsRef.current?.contains(event.target as Node)) {
        setVersionsOpen(false);
      }
    };
    const onKey = (event: KeyboardEvent): void => {
      if (event.key === "Escape") setVersionsOpen(false);
    };
    document.addEventListener("mousedown", onDown);
    document.addEventListener("keydown", onKey);
    return () => {
      document.removeEventListener("mousedown", onDown);
      document.removeEventListener("keydown", onKey);
    };
  }, [versionsOpen]);

  const load = useCallback(async () => {
    try {
      const payload = await apiGet<RecordPayload>(`/api/dashboards/${id}`);
      setDoc(payload.record.doc);
      setSaved(JSON.stringify(payload.record.doc));
      setVersions(payload.versions);
      setLatestVersion(payload.record.latestVersion);
      setError("");
    } catch (caught) {
      setError(caught instanceof ApiError ? caught.message : "The tower did not answer");
    }
  }, [id]);

  useEffect(() => {
    void load();
  }, [load]);

  // Live source data, so the preview shows what would actually ship right now.
  const refreshSources = useCallback(async () => {
    try {
      const payload = await apiGet<{ sources: DashboardSources }>(
        `/api/dashboards/${id}/sources`,
      );
      setSources(payload.sources);
      setNow(new Date());
    } catch {
      // Sources are optional in the designer: an unavailable source is a valid
      // thing to be laying out against, and the modules render it honestly.
    }
  }, [id]);

  useEffect(() => {
    void refreshSources();
  }, [refreshSources]);

  const problems = useMemo(() => (doc ? layoutProblems(doc) : []), [doc]);

  /**
   * Overflowing text, from the very same render the preview shows.
   *
   * Not a second opinion computed a different way: the renderer reports what
   * it actually had to mark in red, so the warning here and the mark on the
   * panel can never disagree.
   */
  const rendered = useMemo<{
    overflows: DashboardOverflow[];
    notes: DashboardNote[];
    contrasts: DashboardContrast[];
    remapped: number;
  }>(() => {
    const empty = { overflows: [], notes: [], contrasts: [], remapped: 0 };
    if (!doc) return empty;
    try {
      const report = new RenderReport();
      renderDashboard(doc, sources, { now, timeZone: PANEL_TIMEZONE }, report);
      return {
        overflows: report.overflows,
        notes: report.notes,
        contrasts: report.contrasts,
        remapped: report.remappedPixels,
      };
    } catch {
      // A render that throws is already surfaced by the preview itself.
      return empty;
    }
  }, [doc, sources, now]);
  const overflows = rendered.overflows;
  const dirty = doc !== null && JSON.stringify(doc) !== saved;
  const selected = doc?.modules.find((module) => module.id === selectedId) ?? null;

  const warningCount =
    problems.length +
    (overflows.length > 0 ? 1 : 0) +
    (rendered.contrasts.length > 0 ? 1 : 0) +
    (rendered.remapped > 0 ? 1 : 0);

  function updateModule(moduleId: string, patch: Partial<ModuleInstance>): void {
    setDoc((current) =>
      current
        ? {
            ...current,
            modules: current.modules.map((module) =>
              module.id === moduleId ? { ...module, ...patch } : module,
            ),
          }
        : current,
    );
  }

  /**
   * Add a module, and select it.
   *
   * Everything here happens outside the state updater on purpose. It used to
   * mint the id and call `setSelectedId` *inside* the `setDoc` callback, which
   * React is entitled to invoke more than once for the same update — and does,
   * in development. Two invocations minted two ids: the document kept one and
   * the selection pointed at the other, so a module appeared on the canvas
   * with the inspector still saying "select a module on the canvas".
   */
  function addModule(type: string): void {
    if (!doc) return;
    const definition = moduleDefinition(type);
    const slot = findFreeSlot(doc, definition.defaultSpan);
    if (!slot) {
      setError(
        `There is no free ${definition.defaultSpan.w}x${definition.defaultSpan.h} space left on the grid`,
      );
      return;
    }
    const module: ModuleInstance = {
      id: newModuleId(),
      type,
      x: slot.x,
      y: slot.y,
      w: definition.defaultSpan.w,
      h: definition.defaultSpan.h,
      hidden: false,
      options: { ...(definition.defaultOptions as Record<string, unknown>) },
    };
    // On a narrow screen the module palette is below the canvas, so pressing
    // one of its buttons has scrolled the canvas off the top of the screen —
    // and the tile that was just added with it. Bring it back: the point of
    // adding a module is to see where it landed.
    //
    // The commit is forced first, and the scroll happens inside the press
    // rather than on the next animation frame. Both halves of that matter:
    //
    //  - `flushSync`, because the tile and any warning line the new tile
    //    raises have to be in the document before the stage is measured;
    //    scrolling to a stage whose position a pending render is about to
    //    change reveals the wrong pixels.
    //  - not `requestAnimationFrame`, because that does not mean "in a
    //    moment". It means "the next time this tab paints", and the paint
    //    after an add is a full 400x300 render of the panel. On a phone that
    //    put the scroll hundreds of milliseconds later, by which time the
    //    person may have tabbed to the new tile and started nudging it with
    //    the arrow keys — and the page then jumped under a live interaction.
    //    A scroll caused by a press belongs to that press.
    flushSync(() => {
      setError("");
      setSelectedId(module.id);
      setDoc({ ...doc, modules: [...doc.modules, module] });
    });
    if (singleColumn) {
      stageRef.current?.scrollIntoView({ block: "start", behavior: "auto" });
    }
    toast(`Added “${definition.label}” in the free slot`);
  }

  function removeModule(moduleId: string): void {
    setDoc((current) =>
      current
        ? { ...current, modules: current.modules.filter((m) => m.id !== moduleId) }
        : current,
    );
    setSelectedId(null);
  }

  function beginDrag(
    event: React.PointerEvent,
    module: ModuleInstance,
    mode: "move" | "resize",
  ): void {
    event.preventDefault();
    event.stopPropagation();
    (event.target as Element).setPointerCapture?.(event.pointerId);
    setSelectedId(module.id);
    dragRef.current = {
      moduleId: module.id,
      mode,
      startX: event.clientX,
      startY: event.clientY,
      origin: { ...module },
    };
  }

  function onPointerMove(event: React.PointerEvent): void {
    const drag = dragRef.current;
    if (!drag || !doc) return;

    const cell = 50 * zoom;
    const dx = Math.round((event.clientX - drag.startX) / cell);
    const dy = Math.round((event.clientY - drag.startY) / cell);

    // All the clamping and the overlap rule live in the model, so the designer
    // and the renderer can never disagree about where a module ended up.
    const next = applyDrag(doc, drag.origin, drag.mode, dx, dy);
    if (next) updateModule(next.id, next);
  }

  function endDrag(): void {
    dragRef.current = null;
  }

  /**
   * The keyboard path through the layout.
   *
   * Before this, a module could only be moved or resized with a pointer, which
   * made the one screen this product exists for unusable from a keyboard. The
   * rules are not re-implemented here: arrows and Shift+arrows go through the
   * same `applyDrag` as a mouse drag, so an overlap is refused identically and
   * the clamping is the model's, not the view's.
   */
  function onModuleKeyDown(event: React.KeyboardEvent, module: ModuleInstance): void {
    if (!doc) return;
    const deltas: Record<string, [number, number]> = {
      ArrowLeft: [-1, 0],
      ArrowRight: [1, 0],
      ArrowUp: [0, -1],
      ArrowDown: [0, 1],
    };

    if (event.key === "Delete" || event.key === "Backspace") {
      event.preventDefault();
      setConfirmDelete(module.id);
      return;
    }

    const delta = deltas[event.key];
    if (!delta) return;
    event.preventDefault();

    const mode = event.shiftKey ? "resize" : "move";
    const next = applyDrag(doc, module, mode, delta[0], delta[1]);
    if (!next) {
      setAnnouncement(
        `${moduleDefinition(module.type).label}: that ${mode === "resize" ? "size" : "move"} would overlap another module, so it did not land.`,
      );
      return;
    }
    updateModule(next.id, next);
    setAnnouncement(
      `${moduleDefinition(next.type).label}: column ${next.x}, row ${next.y}, ${next.w} by ${next.h} cells.`,
    );
  }

  async function save(): Promise<void> {
    if (!doc) return;
    setBusy(true);
    setError("");
    try {
      const payload = await apiSend<{
        record: { doc: DashboardDoc; latestVersion: number };
      }>(`/api/dashboards/${id}/versions`, "POST", { doc, note: "" });
      setSaved(JSON.stringify(payload.record.doc));
      setDoc(payload.record.doc);
      setLatestVersion(payload.record.latestVersion);
      await load();
      toast(
        `Saved as v${payload.record.latestVersion} — previous versions are kept`,
      );
    } catch (caught) {
      setError(caught instanceof ApiError ? caught.message : "The version could not be saved");
    } finally {
      setBusy(false);
    }
  }

  async function rollback(version: number): Promise<void> {
    setBusy(true);
    setError("");
    setVersionsOpen(false);
    try {
      await apiSend(`/api/dashboards/${id}/rollback`, "POST", { version });
      await load();
      toast(`Restored v${version} as a new version — nothing was deleted`);
    } catch (caught) {
      setError(caught instanceof ApiError ? caught.message : "The rollback failed");
    } finally {
      setBusy(false);
    }
  }

  if (!doc) {
    return (
      <>
        <div className="page-head">
          <h1>Composition</h1>
        </div>
        {error ? (
          <PageError message={error} onRetry={() => void load()} />
        ) : (
          <PageLoading label="Loading the composition." rows={2} />
        )}
      </>
    );
  }

  const padding = (doc.theme ?? DEFAULT_THEME).contentPadding;
  const content = contentRect(padding);
  const canSave = !busy && dirty && problems.length === 0;

  const zoomLabel =
    zoomMode === "fit" ? "Zoom to 1:1" : zoomMode === 1 ? "Zoom to 2x" : "Fit to width";
  const cycleZoom = () =>
    setZoomMode((mode) => (mode === "fit" ? 1 : mode === 1 ? 2 : "fit"));

  const reading = device.input
    ? {
        input: device.input,
        observed: device.observed ?? true,
        nextWakeLabel: device.nextWakeLabel ?? null,
        address: device.address ?? null,
        simulated: device.simulated ?? true,
      }
    : null;

  const inspector = selected ? (
    <>
      <div className="inspector-head">
        <h2>{moduleDefinition(selected.type).label}</h2>
        <span className="mono-meta">
          {selected.w} × {selected.h} cells
        </span>
      </div>
      {selected.hidden && (
        <Banner tone="info" testId="module-hidden-note">
          <span>
            Hidden. It keeps its place and its wording and draws nothing.
          </span>
        </Banner>
      )}
      <ModuleInspector
        module={selected}
        problems={problems.filter((problem) => problem.includes(selected.id))}
        overflows={overflows.filter((entry) => entry.moduleId === selected.id)}
        notes={rendered.notes.filter((entry) => entry.moduleId === selected.id)}
        contrasts={rendered.contrasts.filter((entry) => entry.moduleId === selected.id)}
        theme={doc.theme ?? DEFAULT_THEME}
        sources={sources}
        now={now}
        onChange={(options) => updateModule(selected.id, { options })}
      />
      <div className="inspector-foot">
        <Button
          className="btn-grow"
          onClick={() => updateModule(selected.id, { hidden: !selected.hidden })}
          testId="toggle-module-hidden"
        >
          {selected.hidden ? "Unhide" : "Hide"}
        </Button>
        <Button
          variant="danger"
          className="btn-grow"
          // Straight to it, with no confirmation, and that is deliberate: a
          // delete here edits a draft. Nothing is written until Save, the
          // previous version is untouched in the history, and a dialog in
          // front of an action that costs a reload to undo is a dialog people
          // learn to dismiss. The keyboard path DOES confirm, because Delete
          // is one keystroke away from the arrow keys that move a block.
          onClick={() => removeModule(selected.id)}
          testId="remove-module"
        >
          Delete
        </Button>
      </div>
    </>
  ) : null;

  return (
    <>
      {/* ------------------------------------------------------- header -- */}
      <div className="editor-head">
        <div className="editor-title">
          <Link className="btn" href="/dashboards" data-testid="back-to-gallery">
            ← Back
          </Link>
          <div className="version-anchor" ref={versionsRef}>
            <h1 data-testid="designer-title">{doc.title}</h1>
            <button
              type="button"
              className="version-toggle"
              aria-expanded={versionsOpen}
              onClick={() => setVersionsOpen((value) => !value)}
              data-testid="version-toggle"
            >
              v{latestVersion} · {dirty ? "unsaved changes" : "saved"}{" "}
              <span aria-hidden="true">▾</span>
            </button>

            {/*
              Always in the document, hidden with `display: none` when closed.
              Not a conditional render, and the difference is the accessibility
              tree: `display: none` takes the whole list out of it and out of
              the tab order, which is the same thing a conditional render would
              achieve, while leaving the version history readable to anything
              that inspects the page rather than looks at it.
            */}
            <div
              className="version-menu-wrap"
              data-open={versionsOpen}
              aria-hidden={!versionsOpen}
            >
              <ul className="version-menu" data-testid="version-rail">
                {versions.length === 0 && (
                  <li className="version-row">
                    <span className="mono-meta">No versions saved yet.</span>
                  </li>
                )}
                {versions.map((version) => (
                  <li className="version-row" key={version.version}>
                    <span className="version-number">v{version.version}</span>
                    <span className="mono-meta version-when">
                      {relativeTime(version.savedAt)}
                      {version.rolledBackFrom !== null
                        ? ` · from v${version.rolledBackFrom}`
                        : ""}
                      {version.note ? ` · ${version.note}` : ""}
                    </span>
                    {version.version === latestVersion ? (
                      <span className="version-current">CURRENT</span>
                    ) : (
                      <Button
                        disabled={busy}
                        onClick={() => void rollback(version.version)}
                        testId={`rollback-${version.version}`}
                      >
                        Restore
                      </Button>
                    )}
                  </li>
                ))}
              </ul>
              <p className="mono-note version-note">
                Saves are kept. Restoring copies a version forward as a new one;
                nothing is deleted.
              </p>
            </div>
          </div>
        </div>

        <div className="card-actions">
          <Button onClick={cycleZoom} testId="zoom-toggle">
            {zoomLabel}
          </Button>
          <Button
            onClick={() => void save()}
            disabled={!canSave}
            testId="save-version"
          >
            Save
          </Button>
          <Button
            variant="primary"
            onClick={() => setSendOpen(true)}
            disabled={busy}
            testId="show-on-panel"
          >
            Show on panel
          </Button>
        </div>
      </div>

      {/*
        A one-line count above the stage, and the warnings themselves below it.
        The stage stays the first thing on the page at every width, which is the
        actual defect: four full-width banners used to sit between the title and
        the canvas, and on a phone the canvas — the reason the page exists —
        started below the fold.
      */}
      {warningCount > 0 && (
        <p className="warning-line" data-testid="warning-count">
          <Badge kind="pending">
            {warningCount} warning{warningCount === 1 ? "" : "s"}
          </Badge>
          <span className="field-hint">
            Shown in full below the canvas. Only a layout problem blocks a save.
          </span>
        </p>
      )}

      <ErrorNote>{error}</ErrorNote>

      <div className="designer">
        <div className="designer-main">
          <div className="stage-wrap" data-fit={zoomMode === "fit"}>
            {/* Measures the width the stage may use. See the effect above. */}
            <div className="stage-measure" ref={columnRef} aria-hidden="true" />
            <div
              className="stage"
              ref={stageRef}
              style={{ width: PANEL_W * zoom, height: 300 * zoom }}
              onPointerMove={onPointerMove}
              onPointerUp={endDrag}
              onPointerCancel={endDrag}
              data-testid="designer-stage"
            >
              <FramePreview
                doc={doc}
                sources={sources}
                now={now}
                scale={zoom}
                testId="designer-preview"
              />
              {/*
                The overlay is chrome and sits over the content area, at the
                average cell size. The module boxes below it are not: they are
                placed by the same cellsToPixels the renderer uses, so what is
                dragged is exactly what is painted even when padding makes two
                columns differ by a pixel.
              */}
              <div
                className="grid-overlay"
                style={{
                  left: content.x * zoom,
                  top: content.y * zoom,
                  width: content.w * zoom,
                  height: content.h * zoom,
                  backgroundSize: `${(content.w / 8) * zoom}px ${(content.h / 6) * zoom}px`,
                }}
              />
              {/*
                The content boundary, dashed.
                Not a decorative inset and not an invented "safe area": it is
                `contentRect(theme.contentPadding)`, the rectangle the renderer
                actually lays the eight-by-six grid inside. With the default
                padding of zero it sits on the panel edge, which is the truth;
                raise the padding in the theme panel and it moves, because the
                modules move with it.
              */}
              <div
                className="content-bound"
                aria-hidden="true"
                style={{
                  left: content.x * zoom,
                  top: content.y * zoom,
                  width: content.w * zoom,
                  height: content.h * zoom,
                }}
              />
              {doc.modules.map((module) => {
                const rect = cellsToPixels(
                  module.x,
                  module.y,
                  module.w,
                  module.h,
                  padding,
                );
                const hasOverflow = overflows.some(
                  (entry) => entry.moduleId === module.id,
                );
                return (
                <div
                  key={module.id}
                  className="module-box"
                  data-selected={module.id === selectedId}
                  data-hidden={module.hidden}
                  data-overflow={hasOverflow}
                  data-testid={`module-${module.type}`}
                  data-module-id={module.id}
                  style={{
                    left: rect.x * zoom,
                    top: rect.y * zoom,
                    width: rect.w * zoom,
                    height: rect.h * zoom,
                  }}
                  // Tab reaches every module in document order; arrows move it,
                  // Shift+arrows resize it, Delete asks first.
                  tabIndex={0}
                  role="button"
                  aria-label={`${moduleDefinition(module.type).label}, column ${module.x}, row ${module.y}, ${module.w} by ${module.h} cells`}
                  onFocus={() => setSelectedId(module.id)}
                  onKeyDown={(event) => onModuleKeyDown(event, module)}
                  onPointerDown={(event) => beginDrag(event, module, "move")}
                >
                  <span className="tag">
                    {module.type}
                    {module.hidden ? " · hidden" : ""}
                  </span>
                  {hasOverflow && (
                    <span
                      className="overflow-flag"
                      title="Text does not fit — marked in red on the panel"
                      aria-hidden="true"
                    >
                      ▲
                    </span>
                  )}
                  <span
                    className="handle"
                    data-testid={`resize-${module.type}`}
                    onPointerDown={(event) => beginDrag(event, module, "resize")}
                  />
                </div>
                );
              })}
            </div>

            <div className="stage-captions">
              <span className="mono-meta">
                400 × 300 · dashed line = content area
              </span>
              <span className="mono-meta">exactly what the panel receives</span>
            </div>
          </div>

          {/* What the keyboard just did, for a reader who cannot see the move. */}
          <p className="stage-live" role="status" aria-live="polite" data-testid="stage-live">
            {announcement}
          </p>

          {/* ------------------------------------------ the component gallery --
              Every module the registry knows, each shown as a real render of
              itself rather than a text button. Data-driven from MODULE_TYPES,
              so the gallery and the renderer can never disagree about what this
              product can draw. */}
          <ComponentGallery theme={doc.theme ?? DEFAULT_THEME} onAdd={addModule} />

          <p className="field-hint">
            8 × 6 grid of 50 px cells. Drag to move, drag the corner to resize;
            overlaps never land. Keyboard: Tab to a block, arrows move,
            Shift+arrows resize, Delete removes.
          </p>

          {problems.length > 0 && (
            <Banner tone="attention" testId="layout-problems">
              <span>{problems.join(". ")}</span>
            </Banner>
          )}

          {overflows.length > 0 && (
            <p className="inline-warning" data-testid="overflow-warnings">
              <span className="inline-warning-flag" aria-hidden="true">
                ▲
              </span>
              {overflows.length === 1
                ? "One line does not fit its tile"
                : `${overflows.length} lines do not fit their tiles`}
              {" — marked in red on the panel, not cut. You can still send."}
            </p>
          )}

          {rendered.contrasts.length > 0 && (
            <p className="inline-warning" data-testid="contrast-warnings">
              <span className="inline-warning-flag" aria-hidden="true">
                ▲
              </span>
              A colour cannot be read on the panel — it is drawn as chosen, not
              corrected.
            </p>
          )}

          {rendered.remapped > 0 && (
            <p className="inline-warning inline-warning-quiet" data-testid="palette-remap-note">
              <span className="inline-warning-flag" aria-hidden="true">
                ●
              </span>
              A pigment is off in this theme, so {rendered.remapped} pixels use
              the fallback colour.
            </p>
          )}

          {/* Below the scene now, not above it: a schedule is not what you came
              to this page to look at. */}
          <Card title="Automatic refresh" variant="plain">
            <label className="field">
              <span>Refresh sources and send this composition</span>
              <select
                value={doc.refreshIntervalMinutes ?? ""}
                onChange={(event) => {
                  const value = event.target.value;
                  setDoc({ ...doc, refreshIntervalMinutes: value === "" ? null : Number(value) as 5 | 15 | 30 | 60 | 180 });
                }}
                data-testid="refresh-interval"
              >
                <option value="">Disabled</option>
                <option value="5">Every 5 minutes</option>
                <option value="15">Every 15 minutes</option>
                <option value="30">Every 30 minutes</option>
                <option value="60">Every hour</option>
                <option value="180">Every 3 hours</option>
              </select>
            </label>
            <p className="field-hint">
              Runs only while this composition is selected and the tower is
              connected to the real NOTE4C. Each run refreshes its sources, then
              takes the usual deduplication and unresolved-send gates.
            </p>
            <Button variant="quiet" onClick={() => void refreshSources()} disabled={busy}>
              Refresh sources now
            </Button>
          </Card>
        </div>

        <div className="stack">
          {/*
            One inspector, in one place. On a wide screen it is this card; on a
            narrow one it is the sheet below, and the card is not rendered at
            all — so there is never a second copy of these controls editing the
            same module from off-screen.
          */}
          {!singleColumn &&
            (inspector ? (
              <div className="inspector-card" data-testid="inspector-card">
                {inspector}
              </div>
            ) : (
              <div className="inspector-empty" data-testid="inspector-empty">
                Select a block on the preview to edit it.
              </div>
            ))}

          <ThemePanel
            doc={doc}
            sources={sources}
            now={now}
            onChange={(next) => setDoc(next)}
          />
        </div>
      </div>

      {singleColumn && (
        <BottomSheet
          // Never while the delete confirmation is up: one overlay at a time,
          // and the registry in src/ui/overlay.ts says so out loud in dev.
          open={selected !== null && confirmDelete === null && !sendOpen}
          title={selected ? moduleDefinition(selected.type).label : "Block"}
          onClose={() => setSelectedId(null)}
          testId="module-sheet"
        >
          {inspector}
        </BottomSheet>
      )}

      {/* On a phone the header scrolls away with the title, so the save lives
          where the work is. It only appears when there is something to save. */}
      {singleColumn && (
        <StickyActions
          visible={dirty}
          summary={
            <>
              <Badge kind="draft" /> unsaved changes to {doc.title}
            </>
          }
        >
          <Button
            variant="primary"
            onClick={() => void save()}
            disabled={!canSave}
            testId="sticky-save-version"
          >
            Save
          </Button>
        </StickyActions>
      )}

      <SendFlowDialog
        open={sendOpen}
        dashboardId={id}
        dashboardTitle={doc.title}
        device={reading}
        onClose={() => setSendOpen(false)}
        onSettled={() => {}}
      />

      <ConfirmDialog
        open={confirmDelete !== null}
        title="Delete this block?"
        confirmLabel="Delete it"
        body={
          <p>
            It keeps nothing: the block and every word written into it are
            removed from this draft. Saving is a separate step, and the previous
            version stays in the history either way.
          </p>
        }
        onConfirm={() => {
          if (confirmDelete) removeModule(confirmDelete);
          setConfirmDelete(null);
        }}
        onCancel={() => setConfirmDelete(null)}
      />
    </>
  );
}
