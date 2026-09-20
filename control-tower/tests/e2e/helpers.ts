import fs from "node:fs";
import path from "node:path";
import { expect, type Page } from "@playwright/test";
import { E2E_DATA_DIR, E2E_PASSPHRASE } from "../../playwright.config";

export const QA_DIR = path.join("test-results", "qa");

/** Wipe the throwaway data root so each spec starts from a true first run. */
export function resetDataRoot(): void {
  fs.rmSync(E2E_DATA_DIR, { recursive: true, force: true });
}

export async function completeFirstRun(page: Page): Promise<void> {
  await page.goto("/login");
  await expect(page.getByRole("heading", { name: "Welcome" })).toBeVisible();
  await page.getByTestId("passphrase").fill(E2E_PASSPHRASE);
  await page.getByTestId("passphrase-confirm").fill(E2E_PASSPHRASE);
  await page.getByTestId("submit-auth").click();
  await page.waitForURL("**/overview");
}

export async function signIn(page: Page): Promise<void> {
  await page.goto("/login");
  const heading = page.getByRole("heading", { name: "Welcome" });
  if (await heading.isVisible().catch(() => false)) {
    await completeFirstRun(page);
    return;
  }
  await page.getByTestId("passphrase").fill(E2E_PASSPHRASE);
  await page.getByTestId("submit-auth").click();
  await page.waitForURL("**/overview");
}

let unique = 0;

/**
 * A starter composition, created through the API rather than through the page.
 *
 * The gallery used to carry a title field and a Create button, and this helper
 * drove them. It does not any more: new compositions start from the template
 * picker, which is a dialog of five real layouts and is a thing worth testing
 * on its own (tests/e2e/19-templates.spec.ts) rather than a thing every other
 * spec has to walk through to get a fixture. Creating the fixture the way the
 * picker's first template does — `starter: true`, one POST — keeps the setup
 * cost of forty other tests at one request.
 *
 * Titles carry a run-unique suffix so a rerun against a data root that already
 * holds yesterday's compositions can never match the wrong card.
 */
export async function createDashboard(
  page: Page,
  title: string,
): Promise<{ id: string; title: string }> {
  unique += 1;
  const fullTitle = `${title} ${Date.now().toString(36)}-${unique}`;
  await page.goto("/dashboards");
  const csrf = await csrfFrom(page);
  const created = await page.request.post("/api/dashboards", {
    headers: { "content-type": "application/json", "x-csrf-token": csrf },
    data: { title: fullTitle, starter: true },
  });
  expect(created.status(), await created.text()).toBe(201);
  const body = (await created.json()) as { record: { doc: { id: string } } };
  const id = body.record.doc.id;

  // Back to the gallery so the caller sees the card it just made, which is
  // what every caller that does not immediately navigate expects.
  await page.reload();
  await expect(page.getByTestId(`dash-${id}`)).toBeVisible();
  return { id, title: fullTitle };
}

/**
 * Where a module sits, in grid cells, measured against the stage itself.
 *
 * Both rectangles are read inside one `evaluate`, which is the whole point: a
 * cell position is a *difference* between two boxes, so the two boxes have to
 * come from the same layout. Two `boundingBox()` calls are two round trips to
 * the browser with the page free to move in between, and when it did — the
 * designer used to scroll the canvas into view a frame or two after a module
 * was added — the subtraction mixed a pre-scroll stage with a post-scroll
 * module and reported a tile six rows above the grid it was sitting inside.
 * That failure had nothing to say about the designer and everything to say
 * about how it was measured.
 */
export async function cellPosition(
  page: Page,
  testId: string,
): Promise<{ x: number; y: number; w: number; h: number }> {
  await page.getByTestId("designer-stage").waitFor({ state: "visible" });
  await page.getByTestId(testId).waitFor({ state: "visible" });

  const measured = await page.evaluate((target: string) => {
    const stageNode = document.querySelector('[data-testid="designer-stage"]');
    const boxNode = document.querySelector(`[data-testid="${target}"]`);
    if (!stageNode || !boxNode) return null;
    const stage = stageNode.getBoundingClientRect();
    const box = boxNode.getBoundingClientRect();
    return {
      stage: { x: stage.x, y: stage.y, width: stage.width },
      box: { x: box.x, y: box.y, width: box.width, height: box.height },
    };
  }, testId);

  if (!measured) throw new Error(`No box for ${testId}`);
  const { stage, box } = measured;
  const cell = stage.width / 8;
  return {
    x: Math.round((box.x - stage.x) / cell),
    y: Math.round((box.y - stage.y) / cell),
    w: Math.round(box.width / cell),
    h: Math.round(box.height / cell),
  };
}

/**
 * Choose which composition the panel shows next, and stop there.
 *
 * "Show on panel" does two things: it selects, which is a tower-side
 * preference, and it opens the review, which is the first step of reaching the
 * hardware. A spec that only wants the first has to dismiss the second, exactly
 * as a person changing their mind would — and it has to wait for the
 * server-backed SELECTED badge before navigating, because the button's own busy
 * state is not confirmation and without it a later send runs against whatever
 * was selected before.
 */
export async function selectDashboard(page: Page, id: string): Promise<void> {
  await page.getByTestId(`select-${id}`).click();

  // Whatever screen the review reached, leave it. The dialog does not always
  // land on `review`: an outstanding send from an earlier spec puts it on
  // `blocked` instead, which has no Cancel because there is nothing to cancel.
  // The close control in its header is on every screen except `progress`, and
  // `progress` cannot happen here because nothing has been confirmed.
  await expect(page.getByTestId("send-flow")).toBeVisible();
  const cancel = page.getByTestId("send-cancel");
  if (await cancel.isVisible().catch(() => false)) await cancel.click();
  else await page.getByTestId("send-flow-close").click();
  await expect(page.getByTestId("send-flow")).toHaveCount(0);

  await expect(
    page.getByTestId(`dash-${id}`).locator('[data-badge="selected"]'),
  ).toBeVisible();
}

export async function csrfFrom(page: Page): Promise<string> {
  const cookies = await page.context().cookies();
  return cookies.find((cookie) => cookie.name === "note4c_tower_csrf")?.value ?? "";
}

/**
 * A dashboard whose rendered pixels are unique.
 *
 * Two starter dashboards render the identical frame: the title is tower-side
 * metadata and never reaches the panel. That is correct product behaviour and
 * the dedup gate rightly catches it, but a push spec needs a frame the device
 * has genuinely not seen, so this writes a unique message into the layout and
 * saves it as version 2.
 */
export async function createDistinctDashboard(
  page: Page,
  name: string,
): Promise<{ id: string; title: string; version: number }> {
  const { id, title } = await createDashboard(page, name);
  const csrf = await csrfFrom(page);

  const current = await page.request.get(`/api/dashboards/${id}`);
  expect(current.status()).toBe(200);
  const { record } = (await current.json()) as {
    record: { doc: { modules: Array<{ type: string; options: unknown }> } };
  };

  const message = record.doc.modules.find((module) => module.type === "message");
  if (!message) throw new Error("The starter layout has no message module");
  // Schema 2: the note is a text element, not a bare string. Writing the v1
  // shape here would be silently dropped as an unknown key, every spec's
  // dashboard would render the same blank message, and the semantic dedup
  // gate would refuse the second push of the run.
  message.options = {
    ...(message.options as Record<string, unknown>),
    body: { text: title, visible: true },
  };

  const saved = await page.request.post(`/api/dashboards/${id}/versions`, {
    headers: { "content-type": "application/json", "x-csrf-token": csrf },
    data: { doc: record.doc, note: "Unique content for this spec" },
  });
  expect(saved.status(), await saved.text()).toBe(201);
  const body = (await saved.json()) as { record: { latestVersion: number } };

  return { id, title, version: body.record.latestVersion };
}

/** A dashboard with no modules, for layout tests that need a clear grid. */
export async function createEmptyDashboard(
  page: Page,
  name: string,
): Promise<{ id: string; title: string }> {
  unique += 1;
  const title = `${name} ${Date.now().toString(36)}-${unique}`;
  await page.goto("/dashboards");
  const csrf = await csrfFrom(page);
  const created = await page.request.post("/api/dashboards", {
    headers: { "content-type": "application/json", "x-csrf-token": csrf },
    data: { title, starter: false },
  });
  expect(created.status(), await created.text()).toBe(201);
  const body = (await created.json()) as { record: { doc: { id: string } } };
  return { id: body.record.doc.id, title };
}

export interface MockState {
  asleep: boolean;
  stallMs: number;
  failAck: boolean;
  panelDelayMs: number;
  configRevision: number;
  performedActions: Array<{ action: string; atMs: number }>;
  /** Frame writes the mock has received since the last reset. */
  framePuts: number;
  /** The result of a background device pass, when one was asked for. */
  deviceTick: {
    intent: string;
    queued: string;
    reachable: boolean | null;
  } | null;
  /** The mock's own view of itself. */
  snapshot: {
    stored: { seq: number; sha256: string };
    displayed: { seq: number; sha256: string };
    refresh: { state: string; pending: boolean; renders: number };
  };
}

/** Drive the test-only mock control route, which needs NOTE4C_TOWER_E2E=1. */
export async function setMock(
  page: Page,
  body: {
    failAck?: boolean;
    panelDelayMs?: number;
    reset?: boolean;
    bumpRevision?: boolean;
    slideMin?: 0 | 5 | 10 | 30;
    /** Make the mock stop answering, the way a deep-sleeping device does. */
    asleep?: boolean;
    /** Accept the socket and never answer, so the caller pays its timeout. */
    stallMs?: number;
    powerMode?: "auto_saver" | "interactive" | "always_on";
    powerPendingWake?: boolean;
    /** Forget the pairing, so the device answers and refuses permanently. */
    unpair?: boolean;
    /** Run one pass of the background scheduler's device window. */
    runDeviceTick?: boolean;
  },
): Promise<MockState> {
  const csrf = await csrfFrom(page);
  const response = await page.request.post("/api/test-hooks/mock-device", {
    headers: { "content-type": "application/json", "x-csrf-token": csrf },
    data: body,
  });
  expect(response.status(), await response.text()).toBe(200);
  return (await response.json()) as MockState;
}

/** The canvas the designer paints, as a data URL, for before/after compares. */
export async function previewPixels(page: Page): Promise<string> {
  return page.evaluate(() => {
    const canvas = document.querySelector<HTMLCanvasElement>(
      '[data-testid="designer-preview"]',
    );
    return canvas?.toDataURL() ?? "";
  });
}

/**
 * The preview once it has stopped moving.
 *
 * A freshly loaded designer paints twice: once from the document alone, and
 * again when the source read comes back. Comparing pixels across a navigation
 * without waiting for that compares a half-drawn panel with a finished one,
 * which is a flake rather than a finding.
 */
export async function stablePixels(page: Page): Promise<string> {
  let last = await previewPixels(page);
  for (let attempt = 0; attempt < 25; attempt += 1) {
    await page.waitForTimeout(150);
    const next = await previewPixels(page);
    if (next.length > 0 && next === last) return next;
    last = next;
  }
  return last;
}

/**
 * Put the inspector sheet away, if this viewport is using one.
 *
 * On a narrow screen the selected module's options live in a sheet anchored to
 * the bottom of the viewport, which covers the lower half of the page — by
 * design, and the way a sheet on a phone always does. A spec that selects a
 * module and then reaches for something further down the page has to close it
 * first, exactly as a person would.
 */
export async function closeInspectorSheet(page: Page): Promise<void> {
  const sheet = page.getByTestId("module-sheet");
  if (await sheet.isVisible().catch(() => false)) {
    await page.getByTestId("sheet-close").click();
    await expect(sheet).toBeHidden();
  }
}

/**
 * Open a module's single "Fine-tune" fold, where every typographic control and
 * per-role colour now lives. The first inspector surface is words and a
 * picture; type, size, weight, alignment, line gap and colour are one
 * disclosure away, per the redesigned information architecture. Idempotent:
 * only opens a closed fold, so a spec can call it before each control group
 * without toggling it shut.
 */
export async function openFineTune(page: Page): Promise<void> {
  const fold = page.getByTestId("inspector-finetune");
  await fold.scrollIntoViewIfNeeded();
  const open = await fold.evaluate((node) => (node as HTMLDetailsElement).open);
  if (!open) await fold.locator("summary").first().click();
}

/**
 * Open the Theme panel's "Advanced" fold, where content padding, the dashboard
 * font and weight, and the palette switches now live. The Appearance pickers
 * (colour, brush, texture) stay on the first surface; everything technical is
 * behind this one disclosure. Idempotent, like `openFineTune`.
 */
export async function openThemeAdvanced(page: Page): Promise<void> {
  const fold = page.getByTestId("theme-advanced");
  await fold.scrollIntoViewIfNeeded();
  const open = await fold.evaluate((node) => (node as HTMLDetailsElement).open);
  if (!open) await fold.locator("summary").first().click();
}

export async function shot(page: Page, name: string, project: string): Promise<void> {
  fs.mkdirSync(QA_DIR, { recursive: true });
  await page.screenshot({
    path: path.join(QA_DIR, `${project}-${name}.png`),
    fullPage: true,
  });
}

/**
 * Make the next animation frame arbitrarily late, on purpose.
 *
 * A `requestAnimationFrame` callback runs when the browser next paints, which
 * on a busy main thread is not "in a moment" but "some hundreds of
 * milliseconds from now, after whatever the person did next". Work the product
 * defers to a frame is therefore *unordered* with respect to the interaction
 * that follows it — and when that work moves the page, the next interaction
 * happens on ground that shifted underneath it.
 *
 * That is not a hypothetical: it is the race that made the keyboard spec
 * measure a module at row -6 while the model and the live region both said row
 * 1. This turns the lateness into something a spec can state rather than hope
 * about: install it before the page loads, and every frame the page asks for
 * arrives `ms` later than the interaction that asked for it.
 *
 * `__frames` lets a spec wait for exactly the deferred work that exists, with
 * no fixed sleep: when `delivered` has caught up with `requested`, whatever the
 * page put off has happened.
 */
export async function delayAnimationFrames(page: Page, ms = 250): Promise<void> {
  await page.addInitScript((delay: number) => {
    const frames = { requested: 0, delivered: 0 };
    Object.assign(window, { __frames: frames });
    window.requestAnimationFrame = (callback: FrameRequestCallback): number => {
      frames.requested += 1;
      return window.setTimeout(() => {
        frames.delivered += 1;
        callback(performance.now());
      }, delay);
    };
    window.cancelAnimationFrame = (handle: number): void => {
      window.clearTimeout(handle);
    };
  }, ms);
}

/** Wait until every frame the page deferred has actually been delivered. */
export async function settleDeferredFrames(page: Page): Promise<void> {
  await page.waitForFunction(() => {
    const frames = (window as unknown as {
      __frames?: { requested: number; delivered: number };
    }).__frames;
    return !frames || frames.delivered >= frames.requested;
  });
}

/** Where the document is scrolled to, as one atomic read. */
export async function documentScroll(page: Page): Promise<{ x: number; y: number }> {
  return page.evaluate(() => ({
    x: Math.round(window.scrollX),
    y: Math.round(window.scrollY),
  }));
}
