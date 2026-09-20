import { expect, test, type Locator, type Page } from "@playwright/test";
import {
  cellPosition,
  createDashboard,
  createEmptyDashboard,
  delayAnimationFrames,
  documentScroll,
  settleDeferredFrames,
  shot,
  signIn,
} from "./helpers";

test.beforeEach(async ({ page }) => {
  await signIn(page);
});

/** Drag by a whole number of cells, whatever the current zoom is. */
async function dragBy(
  page: Page,
  target: Locator,
  cellsX: number,
  cellsY: number,
): Promise<void> {
  // Scrolled into view first: boundingBox() reports where a thing is, not
  // where it can be clicked, and on a phone the handle can be above the fold
  // after the module palette pulled the page down.
  await target.scrollIntoViewIfNeeded();

  // The grab point and the cell size are read from one layout, for the same
  // reason cellPosition does: two round trips let the page move between them,
  // and a drag that starts from a stale point lands a cell out.
  const measured = await target.evaluate((node: Element) => {
    const stageNode = document.querySelector('[data-testid="designer-stage"]');
    if (!stageNode) return null;
    const box = node.getBoundingClientRect();
    return {
      box: { x: box.x, y: box.y, height: box.height },
      cell: stageNode.getBoundingClientRect().width / 8,
    };
  });
  if (!measured) throw new Error("The element has no box to drag");
  const { box, cell } = measured;

  await page.mouse.move(box.x + 6, box.y + box.height / 2);
  await page.mouse.down();
  await page.mouse.move(
    box.x + 6 + cellsX * cell,
    box.y + box.height / 2 + cellsY * cell,
    { steps: 8 },
  );
  await page.mouse.up();
}

test("opens the designer with a live preview and a version rail", async ({
  page,
}, info) => {
  const { id } = await createDashboard(page, `Designer ${info.project.name}`);
  await page.goto(`/dashboards/${id}/edit`);

  await expect(page.getByTestId("designer-title")).toBeVisible();
  await expect(page.getByTestId("designer-preview")).toBeVisible();
  await expect(page.getByTestId("version-rail")).toContainText("v1");

  // The preview canvas is the real renderer, at exactly 400x300.
  const canvas = page.getByTestId("designer-preview");
  await expect(canvas).toHaveAttribute("width", "400");
  await expect(canvas).toHaveAttribute("height", "300");

  await page.waitForTimeout(800);
  await shot(page, "designer", info.project.name);
});

test("adds a module, edits its options and saves a version", async ({ page }, info) => {
  const { id } = await createDashboard(page, `Add module ${info.project.name}`);
  await page.goto(`/dashboards/${id}/edit`);

  // The starter leaves the bottom right 3x1 free, which is exactly a second
  // sensor tile, so this lands deterministically without clearing space first.
  await page.getByTestId("add-haSensor").click();
  await expect(page.getByTestId("module-haSensor")).toHaveCount(2);
  await expect(page.getByTestId("module-inspector")).toBeVisible();

  // The label is a text element now: its wording, its visibility and its
  // typography are three separate controls on one role.
  await page.getByTestId("opt-label").fill("Greenhouse");
  await expect(page.getByTestId("save-version")).toBeEnabled();
  await page.getByTestId("save-version").click();

  await expect(page.getByTestId("version-rail")).toContainText("v2");
  // Saved, so the draft badge is gone and Save is disabled again.
  await expect(page.getByTestId("save-version")).toBeDisabled();
});

test("moving a module snaps to the grid and persists", async ({ page }, info) => {
  const { id } = await createDashboard(page, `Move ${info.project.name}`);
  await page.goto(`/dashboards/${id}/edit`);

  // Measured in grid cells against the stage, so the assertion says what it
  // means and does not move when page chrome above the canvas reflows.
  const before = await cellPosition(page, "module-timestamp");
  expect(before).toMatchObject({ x: 5, y: 4, w: 3, h: 1 });

  // Row 5 at column 5 is the free slot the starter leaves.
  await dragBy(page, page.getByTestId("module-timestamp"), 0, 1);
  expect(await cellPosition(page, "module-timestamp")).toMatchObject({ x: 5, y: 5 });

  await page.getByTestId("save-version").click();
  await expect(page.getByTestId("version-rail")).toContainText("v2");

  await page.reload();
  expect(await cellPosition(page, "module-timestamp")).toMatchObject({ x: 5, y: 5 });
});

test("resizing respects the module's declared span limits", async ({ page }, info) => {
  // An empty grid, so the only limits in play are the module's own.
  const { id } = await createEmptyDashboard(page, `Resize ${info.project.name}`);
  await page.goto(`/dashboards/${id}/edit`);

  // haSensor: default 3x1, minimum 2x1, maximum 4x2. It lands at 0,0 on an
  // empty grid, which keeps the resize handle on screen at either viewport.
  await page.getByTestId("add-haSensor").click();
  expect(await cellPosition(page, "module-haSensor")).toMatchObject({
    x: 0,
    y: 0,
    w: 3,
    h: 1,
  });

  await dragBy(page, page.getByTestId("resize-haSensor"), 3, 3);
  expect(await cellPosition(page, "module-haSensor")).toMatchObject({ w: 4, h: 2 });
  await expect(page.getByTestId("layout-problems")).toHaveCount(0);

  await dragBy(page, page.getByTestId("resize-haSensor"), -4, -4);
  expect(await cellPosition(page, "module-haSensor")).toMatchObject({ w: 2, h: 1 });
  await expect(page.getByTestId("layout-problems")).toHaveCount(0);
});

test("a module that would overlap does not land", async ({ page }, info) => {
  const { id } = await createDashboard(page, `Overlap ${info.project.name}`);
  await page.goto(`/dashboards/${id}/edit`);

  const before = await cellPosition(page, "module-message");
  expect(before).toMatchObject({ x: 5, y: 3 });

  // Straight up into the weather block, which occupies rows 0 to 2.
  await dragBy(page, page.getByTestId("module-message"), -3, -2);

  // The move simply does not land, and the layout stays valid throughout.
  expect(await cellPosition(page, "module-message")).toMatchObject({ x: 5, y: 3 });
  await expect(page.getByTestId("layout-problems")).toHaveCount(0);
});

test("rolls back to an earlier version without losing history", async ({
  page,
}, info) => {
  const { id } = await createDashboard(page, `Rollback ${info.project.name}`);
  await page.goto(`/dashboards/${id}/edit`);

  await page.getByTestId("module-message").click();
  await page.getByTestId("remove-module").click();
  await page.getByTestId("save-version").click();
  await expect(page.getByTestId("version-rail")).toContainText("v2");
  await expect(page.getByTestId("module-message")).toHaveCount(0);

  // The version history is a popover on the title line now, rather than a card
  // in the sidebar. Opening it is the only added step: every assertion below —
  // including every pixel comparison — is unchanged.
  await page.getByTestId("version-toggle").click();
  await page.getByTestId("rollback-1").click();
  await expect(page.getByTestId("version-rail")).toContainText("v3");
  await expect(page.getByTestId("module-message")).toBeVisible();
  // History is copied forward, never rewritten: v2 is still listed.
  await expect(page.getByTestId("version-rail")).toContainText("v2");
  await expect(page.getByTestId("version-rail")).toContainText("from v1");
});

test("the preview redraws as options change", async ({ page }, info) => {
  const { id } = await createDashboard(page, `Preview ${info.project.name}`);
  await page.goto(`/dashboards/${id}/edit`);

  const canvasPixels = async (): Promise<string> =>
    page.evaluate(() => {
      const canvas = document.querySelector<HTMLCanvasElement>(
        '[data-testid="designer-preview"]',
      );
      return canvas?.toDataURL() ?? "";
    });

  await page.getByTestId("module-message").click();
  const before = await canvasPixels();
  await page.getByTestId("opt-body").fill("A new note for the kitchen panel");
  await expect(page.getByTestId("save-version")).toBeEnabled();
  await page.waitForTimeout(300);
  const after = await canvasPixels();

  expect(after).not.toBe(before);
  expect(after.length).toBeGreaterThan(1000);
});

/**
 * Adding a module may not move the page after the fact.
 *
 * On a narrow screen the module palette is below the canvas, so pressing one of
 * its buttons has already scrolled the canvas off the top of the screen — and
 * the designer rightly brings it back, because the point of adding a module is
 * to see where it landed. The *timing* of that scroll is the defect this pins
 * down. It used to be handed to `requestAnimationFrame`, which does not mean
 * "in a moment": it means "the next time this tab paints", and the paint after
 * an add is a full 400x300 render of the panel. On a phone viewport that put
 * the scroll hundreds of milliseconds later — after the person had already
 * tabbed to the new tile and started nudging it with the arrow keys, at which
 * point the page jumped under them and the keyboard spec measured the tile six
 * rows above the stage while the model and the live region both said row one.
 *
 * So the rule, stated rather than hoped for: the reveal belongs to the press
 * that caused it. Frames are delayed here on purpose, which is the only way to
 * assert an ordering instead of observing whichever order happened to occur.
 */
test("adding a module reveals the canvas in the same press, not on a later frame", async ({
  page,
}, info) => {
  await delayAnimationFrames(page);
  const { id } = await createEmptyDashboard(page, `Reveal ${info.project.name}`);
  await page.goto(`/dashboards/${id}/edit`);
  await expect(page.getByTestId("designer-stage")).toBeVisible();

  // Reach the palette the way a person does on a phone: scroll down to it.
  const add = page.getByTestId("add-haSensor");
  await add.scrollIntoViewIfNeeded();
  await add.click();
  await expect(page.getByTestId("module-haSensor")).toBeVisible();

  const settled = await documentScroll(page);
  const viewport = page.viewportSize() ?? { width: 1440, height: 900 };
  const narrow = viewport.width <= 900;

  if (narrow) {
    // The canvas is back, already, with the press over.
    const stage = await page.getByTestId("designer-stage").boundingBox();
    if (!stage) throw new Error("The stage has no box");
    // One pixel of tolerance, the same allowance the horizontal-overflow
    // assertions make. The stage is scaled to a fractional height — a 400x300
    // panel fitted to an arbitrary column width rarely lands on whole pixels —
    // so `scrollIntoView` can settle a third of a pixel either side of the top.
    // The property is "the canvas is back on screen", not "y is exactly zero".
    expect(stage.y).toBeGreaterThanOrEqual(-1);
    expect(stage.y).toBeLessThan(viewport.height / 2);
  }

  // And nothing the page put off may move it now. This waits for exactly the
  // deferred work that exists rather than for a guessed interval: if the
  // designer deferred nothing, there is nothing to wait for.
  await settleDeferredFrames(page);
  expect(await documentScroll(page)).toEqual(settled);
});
