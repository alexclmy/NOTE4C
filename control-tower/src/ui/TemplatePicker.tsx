"use client";

import { useMemo, useState } from "react";
import { useRouter } from "next/navigation";
import {
  emptyDashboard,
  newModuleId,
  type DashboardDoc,
  type ModuleFrame,
  type ModuleInstance,
} from "@/core/model";
import type { Expression } from "@/core/theme";
import { emptySources } from "@/core/render/data";
import { MODULES } from "@/core/render/modules";
import { ApiError, apiSend } from "./api";
import { Button } from "./components";
import { Dialog } from "./Dialog";
import { FramePreview } from "./FramePreview";

/**
 * A handful of complete, expressive dashboards to start from.
 *
 * Each is a real layout of real modules with a look already chosen, not a
 * utilitarian skeleton: a morning board for the fridge, an editorial note, a
 * weather poster. The thumbnails are not drawings of layouts — each one is the
 * actual renderer running over the actual document the button would create, at
 * the actual four pigments and the template's own appearance. So what somebody
 * picks is exactly what they get, and a hand-drawn approximation could not
 * promise that.
 *
 * They render against `emptySources()` — no weather, no calendar, no sensor —
 * which is what a person sees before configuring anything and what the modules
 * draw for an unavailable source. A thumbnail showing a live 18° would be
 * promising data this composition does not yet have.
 *
 * Every template is editable afterwards; none of them is a mode.
 */

interface Template {
  key: string;
  name: string;
  desc: string;
  /**
   * Built lazily, and with a fixed clock, so the five previews do not re-render
   * every time this component does.
   */
  build: (now: Date) => DashboardDoc;
}

/** Place a module of a known type at a cell rectangle, with its own defaults. */
function place(
  type: string,
  x: number,
  y: number,
  w: number,
  h: number,
  options: Record<string, unknown> = {},
): ModuleInstance {
  return {
    id: newModuleId(),
    type,
    x,
    y,
    w,
    h,
    hidden: false,
    options: {
      ...(MODULES[type]?.defaultOptions as Record<string, unknown>),
      ...options,
    },
  };
}

/**
 * Give a placed module a frame. A rule (or rules) on the named edges is what
 * delimits it from whatever abuts it — the structural device that makes a
 * multi-module composition read as designed rather than as blocks that happen
 * to touch. `edges` is the only required part; weight/style default sensibly.
 */
function framed(
  module: ModuleInstance,
  edges: ModuleFrame["edges"],
  extra: Partial<Omit<ModuleFrame, "edges">> = {},
): ModuleInstance {
  return {
    ...module,
    frame: {
      edges,
      weight: extra.weight ?? 2,
      style: extra.style ?? "solid",
      inset: extra.inset ?? 0,
      color: extra.color ?? 0,
    },
  };
}

/**
 * A text element, in the shape the schema actually stores.
 *
 * Schema 2 moved every piece of wording from a bare string to `{ text,
 * visible }` plus its own typography. Writing the old shape here would be
 * silently dropped as an unknown key and every template would come out with
 * empty wording — which is precisely the bug the e2e helper's comment records
 * from the last time somebody wrote a `string` where an element belonged.
 */
function text(value: string): { text: string; visible: boolean } {
  return { text: value, visible: true };
}

/** A composition with a look already chosen. Modules are laid out by `build`. */
function expressive(
  doc: DashboardDoc,
  expression: Partial<Expression>,
): DashboardDoc {
  return {
    ...doc,
    theme: {
      ...doc.theme,
      expression: { ...doc.theme.expression, ...expression },
    },
  };
}

const TEMPLATES: readonly Template[] = [
  {
    key: "weatherAgenda",
    name: "Weather & agenda",
    desc: "A room-readable temperature beside the day's events, split by a column rule.",
    build: (now) =>
      expressive(
        {
          ...emptyDashboard("Weather & agenda", now),
          modules: [
            // The hero owns the left half; its right edge is the rule that
            // separates weather from agenda. The agenda column stacks the
            // events, the date, and a short note, ruled off from each other.
            framed(place("weatherHero", 0, 0, 5, 6), ["right"]),
            framed(place("calendarNext", 5, 0, 3, 4), ["bottom"]),
            framed(place("timestamp", 5, 4, 3, 1), ["bottom"]),
            place("message", 5, 5, 3, 1, { body: text("À la maison") }),
          ],
        },
        { colourUse: "expressive" },
      ),
  },
  {
    key: "fridge",
    name: "Fridge morning",
    desc: "Weather over the day's events, with a sky to the side.",
    build: (now) =>
      expressive(
        {
          ...emptyDashboard("Fridge morning", now),
          modules: [
            framed(place("weather24h", 0, 0, 8, 3), ["bottom"]),
            framed(place("calendarNext", 0, 3, 5, 3), ["right"]),
            place("sky", 5, 3, 3, 3),
          ],
        },
        { colourUse: "balanced" },
      ),
  },
  {
    key: "editorial",
    name: "Editorial note",
    desc: "A headline over a message, set like a front page.",
    build: (now) =>
      expressive(
        {
          ...emptyDashboard("Editorial note", now),
          modules: [
            framed(
              place("headline", 0, 0, 8, 3, {
                variant: "banner",
                kicker: text("AUJOURD'HUI"),
                headline: text("Bonjour"),
                subline: text("Le beau temps revient"),
              }),
              ["bottom"],
            ),
            place("message", 0, 3, 8, 2, {
              body: {
                text: "Une note pour la maison",
                visible: true,
                style: { size: 22, align: "center" },
              },
            }),
            place("timestamp", 0, 5, 3, 1),
          ],
        },
        { colourUse: "expressive", brush: "grid" },
      ),
  },
  {
    key: "weatherPoster",
    name: "Weather poster",
    desc: "A full sky, with the octopus and the next 24 hours below.",
    build: (now) =>
      expressive(
        {
          ...emptyDashboard("Weather poster", now),
          modules: [
            place("sky", 0, 0, 8, 4, { variant: "arc" }),
            framed(place("octopus", 0, 4, 2, 2), ["top", "right"]),
            framed(place("weather24h", 2, 4, 6, 2), ["top"]),
          ],
        },
        { colourUse: "expressive" },
      ),
  },
  {
    key: "skyAgenda",
    name: "Sky & agenda",
    desc: "A low sky band over the next events.",
    build: (now) =>
      expressive(
        {
          ...emptyDashboard("Sky & agenda", now),
          modules: [
            framed(place("sky", 0, 0, 8, 3, { variant: "horizon" }), ["bottom"]),
            place("calendarNext", 0, 3, 8, 3),
          ],
        },
        { colourUse: "balanced" },
      ),
  },
  {
    key: "photoDay",
    name: "Photo & day",
    desc: "A tall photo beside the day's events and time.",
    build: (now) =>
      expressive(
        {
          ...emptyDashboard("Photo & day", now),
          modules: [
            framed(place("image", 0, 0, 4, 6), ["right"]),
            framed(place("calendarNext", 4, 0, 4, 4), ["bottom"]),
            framed(place("timestamp", 4, 4, 4, 1), ["bottom"]),
            place("message", 4, 5, 4, 1, { body: text("Aujourd'hui") }),
          ],
        },
        { colourUse: "blackwhite" },
      ),
  },
  {
    key: "blank",
    name: "Blank",
    desc: "An empty 400 × 300 canvas.",
    build: (now) => emptyDashboard("Blank", now),
  },
];

export function TemplatePicker({
  open,
  onClose,
  onCreated,
}: {
  open: boolean;
  onClose: () => void;
  onCreated?: (name: string) => void;
}) {
  const router = useRouter();
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");

  // One clock for all five, fixed for the life of this dialog: the timestamp
  // module draws the render time, and a `new Date()` per render would repaint
  // five canvases every second.
  const now = useMemo(() => new Date(), []);
  const sources = useMemo(() => emptySources(), []);
  const docs = useMemo(
    () => TEMPLATES.map((template) => template.build(now)),
    [now],
  );

  async function choose(index: number): Promise<void> {
    const template = TEMPLATES[index];
    const doc = docs[index];
    if (!template || !doc) return;
    setBusy(true);
    setError("");
    try {
      /*
       * The composition is created empty — a blank v1 the product keeps in the
       * history rather than tidying away — and then the template's layout is
       * written as the working document and saved as v2. "v1 was the blank
       * canvas this started from" is true and worth being able to roll back to,
       * and this product never rewrites a version list.
       */
      const created = await apiSend<{ record: { doc: { id: string } } }>(
        "/api/dashboards",
        "POST",
        { title: template.name, starter: false },
      );
      const id = created.record.doc.id;

      if (doc.modules.length > 0) {
        // The server minted the id and the timestamps; only the layout is
        // ours to contribute, so the document that goes back is the server's
        // with our modules in it rather than the one built above wholesale.
        const current = await apiSend<{ record: { doc: DashboardDoc } }>(
          `/api/dashboards/${id}`,
          "PUT",
          { doc: { ...doc, id, title: template.name } },
        );
        await apiSend(`/api/dashboards/${id}/versions`, "POST", {
          doc: current.record.doc,
          note: `Started from the ${template.name} template`,
        });
      }

      onCreated?.(template.name);
      onClose();
      router.push(`/dashboards/${id}/edit`);
    } catch (caught) {
      setError(
        caught instanceof ApiError
          ? caught.message
          : "The composition could not be created",
      );
    } finally {
      setBusy(false);
    }
  }

  if (!open) return null;

  return (
    <Dialog
      open={open}
      title="New composition"
      subtitle="Start from something that already reads well on the panel. Everything stays editable."
      wide
      id="template-picker"
      testId="template-picker"
      onClose={onClose}
      head={
        <Button
          className="modal-close"
          onClick={onClose}
          ariaLabel="Close"
          testId="template-close"
        >
          ✕
        </Button>
      }
      footer={null}
    >
      {error && (
        <p className="error-note" role="alert">
          {error}
        </p>
      )}
      <div className="template-grid">
        {TEMPLATES.map((template, index) => (
          <button
            type="button"
            className="template-card"
            key={template.key}
            onClick={() => void choose(index)}
            disabled={busy}
            data-testid={`template-${template.key}`}
            {...(index === 0 ? { "data-dialog-autofocus": "" } : {})}
          >
            <span className="template-thumb">
              <FramePreview
                doc={docs[index] as DashboardDoc}
                sources={sources}
                now={now}
                fluid
              />
            </span>
            <span className="template-text">
              <strong>{template.name}</strong>
              <span>{template.desc}</span>
            </span>
          </button>
        ))}
      </div>
    </Dialog>
  );
}
