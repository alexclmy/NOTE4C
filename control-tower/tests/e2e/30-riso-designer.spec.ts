import { expect, test, type Page } from "@playwright/test";
import { createEmptyDashboard, shot, signIn, stablePixels } from "./helpers";

/**
 * The editorial modules and the Appearance controls, in the redesigned
 * designer. This spec exists to exercise — and to photograph — the visual
 * pickers: choosing a disposition and a colour treatment by thumbnail rather
 * than by reading a label.
 */

test.beforeEach(async ({ page }) => {
  await signIn(page);
});

async function openEmptyDesigner(page: Page, name: string): Promise<void> {
  const { id } = await createEmptyDashboard(page, name);
  await page.goto(`/dashboards/${id}/edit`);
  await expect(page.getByTestId("designer-stage")).toBeVisible();
}

test("the Headline module is added and its disposition chosen by thumbnail", async ({
  page,
}, info) => {
  await openEmptyDesigner(page, `Headline ${info.project.name}`);
  await page.getByTestId("add-headline").click();
  await expect(page.getByTestId("module-headline")).toBeVisible();

  // The disposition picker is a row of real renders, not a dropdown of words.
  const picker = page.getByTestId("variant-picker");
  await expect(picker).toBeVisible();
  await expect(page.getByTestId("variant-picker-underline-canvas")).toBeVisible();
  await expect(page.getByTestId("variant-picker-banner-canvas")).toBeVisible();
  await shot(page, "riso-headline-inspector", info.project.name);

  // Choosing a thumbnail changes the panel.
  const before = await stablePixels(page);
  await page.getByTestId("variant-picker-sidebar").click();
  await page.waitForTimeout(250);
  const after = await stablePixels(page);
  expect(after).not.toBe(before);
  await shot(page, "riso-headline-sidebar", info.project.name);
});

test("the Sky module renders sun, arc and moon and folds its coordinates away", async ({
  page,
}, info) => {
  await openEmptyDesigner(page, `Sky ${info.project.name}`);
  await page.getByTestId("add-sky").click();
  await expect(page.getByTestId("module-sky")).toBeVisible();

  // The disposition picker is here too.
  await expect(page.getByTestId("variant-picker")).toBeVisible();
  // The technical knobs — latitude, longitude, timezone — are behind the one
  // "Fine-tune" fold, collapsed by default, so the first surface is the
  // disposition and the title and nothing else.
  await expect(page.getByTestId("inspector-finetune")).toBeVisible();
  await expect(page.getByTestId("opt-latitude")).toBeHidden();
  await shot(page, "riso-sky-inspector", info.project.name);
});

test("the Appearance controls re-skin the whole panel", async ({ page }, info) => {
  await openEmptyDesigner(page, `Appearance ${info.project.name}`);
  await page.getByTestId("add-headline").click();
  await expect(page.getByTestId("module-headline")).toBeVisible();

  // The three Appearance pickers, each a strip of thumbnails.
  await expect(page.getByTestId("expr-colour")).toBeVisible();
  await expect(page.getByTestId("expr-brush")).toBeVisible();
  await expect(page.getByTestId("expr-texture")).toBeVisible();
  await shot(page, "riso-appearance", info.project.name);

  const balanced = await stablePixels(page);
  await page.getByTestId("expr-colour-expressive").click();
  await page.waitForTimeout(250);
  const expressive = await stablePixels(page);
  expect(expressive).not.toBe(balanced);
  await shot(page, "riso-appearance-expressive", info.project.name);

  await page.getByTestId("expr-brush-grid").click();
  await page.waitForTimeout(250);
  const grid = await stablePixels(page);
  expect(grid).not.toBe(expressive);
  await shot(page, "riso-appearance-grid", info.project.name);

  // Black & white spends no warm ink.
  await page.getByTestId("expr-colour-blackwhite").click();
  await page.waitForTimeout(250);
  const bw = await stablePixels(page);
  expect(bw).not.toBe(grid);
});

test("a module's separators are toggled by edge and change the panel", async ({
  page,
}, info) => {
  await openEmptyDesigner(page, `Separators ${info.project.name}`);
  await page.getByTestId("add-headline").click();
  await expect(page.getByTestId("module-headline")).toBeVisible();

  // The Separators section is a fold, collapsed by default. Open it.
  const fold = page.getByTestId("inspector-separators");
  await expect(fold).toBeVisible();
  await fold.locator("summary").click();
  await expect(page.getByTestId("frame-controls")).toBeVisible();

  const rightEdge = page.getByTestId("frame-edge-right");
  await expect(rightEdge).toHaveAttribute("aria-pressed", "false");

  // Adding a rule inks the module's right edge, so the panel changes.
  const before = await stablePixels(page);
  await rightEdge.click();
  await expect(rightEdge).toHaveAttribute("aria-pressed", "true");
  await page.waitForTimeout(250);
  const after = await stablePixels(page);
  expect(after).not.toBe(before);
  await shot(page, "riso-separators", info.project.name);

  // Toggling the only edge back off removes the frame entirely.
  await rightEdge.click();
  await expect(rightEdge).toHaveAttribute("aria-pressed", "false");
  await page.waitForTimeout(250);
  expect(await stablePixels(page)).toBe(before);
});
