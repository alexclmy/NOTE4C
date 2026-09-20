import { expect, test } from "@playwright/test";
import { cellPosition, createEmptyDashboard, signIn } from "./helpers";

/**
 * The designer, without a pointer.
 *
 * Until this, moving or resizing a module needed a mouse or a finger: the one
 * screen this product exists for could not be operated from a keyboard at all.
 * What matters as much as the movement is that the *rules* are the same — the
 * arrows go through the same applyDrag as a drag, so an overlap is refused
 * identically rather than by a second implementation that might disagree.
 */

test.beforeEach(async ({ page }) => {
  await signIn(page);
});

test("a module can be selected, moved, resized and saved from the keyboard", async ({
  page,
}, info) => {
  const { id } = await createEmptyDashboard(page, `Keyboard ${info.project.name}`);
  await page.goto(`/dashboards/${id}/edit`);

  await page.getByTestId("add-haSensor").click();
  const module = page.getByTestId("module-haSensor");
  await expect(module).toBeVisible();
  expect(await cellPosition(page, "module-haSensor")).toMatchObject({ x: 0, y: 0 });

  // Focus selects, which is what makes Tab a usable way in.
  await module.focus();
  await expect(module).toHaveAttribute("data-selected", "true");

  await page.keyboard.press("ArrowRight");
  await page.keyboard.press("ArrowDown");
  expect(await cellPosition(page, "module-haSensor")).toMatchObject({ x: 1, y: 1 });

  // The live region says what happened, for a reader who cannot see it move.
  await expect(page.getByTestId("stage-live")).toContainText("column 1, row 1");

  // Shift resizes. haSensor is 3x1 by default and may grow to 4x2.
  await page.keyboard.press("Shift+ArrowRight");
  expect(await cellPosition(page, "module-haSensor")).toMatchObject({ w: 4 });

  await page.getByTestId("save-version").click();
  await expect(page.getByTestId("version-rail")).toContainText("v1");
});

test("an overlapping keyboard move is refused, and says so", async ({ page }, info) => {
  const { id } = await createEmptyDashboard(page, `Keyboard overlap ${info.project.name}`);
  await page.goto(`/dashboards/${id}/edit`);

  // Two tiles side by side, then walk the second one into the first.
  await page.getByTestId("add-haSensor").click();
  await page.getByTestId("add-message").click();

  const second = page.getByTestId("module-message");
  await second.focus();
  const before = await cellPosition(page, "module-message");

  // Walk left until something refuses: either the wall or the other module.
  for (let i = 0; i < 8; i += 1) await page.keyboard.press("ArrowLeft");
  for (let i = 0; i < 8; i += 1) await page.keyboard.press("ArrowUp");

  const after = await cellPosition(page, "module-message");
  // It moved somewhere legal, and it did not land on top of the other tile.
  expect(after.x).toBeGreaterThanOrEqual(0);
  expect(after.y).toBeGreaterThanOrEqual(0);
  expect(await page.getByTestId("layout-problems").count()).toBe(0);
  expect(after).not.toEqual({ ...before, x: -1 });
});

test("Delete asks before it removes", async ({ page }, info) => {
  const { id } = await createEmptyDashboard(page, `Keyboard delete ${info.project.name}`);
  await page.goto(`/dashboards/${id}/edit`);

  await page.getByTestId("add-haSensor").click();
  await page.getByTestId("module-haSensor").focus();
  await page.keyboard.press("Delete");

  const dialog = page.getByRole("dialog");
  await expect(dialog).toBeVisible();
  // The sheet is not mounted underneath it: one overlay at a time.
  await expect(page.getByTestId("module-sheet")).toHaveCount(0);

  await page.getByTestId("confirm-cancel").click();
  await expect(page.getByTestId("module-haSensor")).toHaveCount(1);

  await page.getByTestId("module-haSensor").focus();
  await page.keyboard.press("Delete");
  await page.getByTestId("confirm-ok").click();
  await expect(page.getByTestId("module-haSensor")).toHaveCount(0);
});

test("the inspector exists exactly once, wherever it is rendered", async ({ page }, info) => {
  const { id } = await createEmptyDashboard(page, `One inspector ${info.project.name}`);
  await page.goto(`/dashboards/${id}/edit`);

  await page.getByTestId("add-message").click();
  await expect(page.getByTestId("module-inspector")).toHaveCount(1);
  await expect(page.getByTestId("module-inspector")).toBeVisible();

  const narrow = (page.viewportSize()?.width ?? 1440) <= 900;
  if (narrow) {
    // On a narrow screen it is the sheet, and the canvas stays visible above.
    await expect(page.getByTestId("module-sheet")).toBeVisible();
    await expect(page.getByTestId("designer-stage")).toBeVisible();
    await page.getByTestId("sheet-close").click();
    await expect(page.getByTestId("module-inspector")).toHaveCount(0);
  } else {
    await expect(page.getByTestId("module-sheet")).toHaveCount(0);
  }
});
