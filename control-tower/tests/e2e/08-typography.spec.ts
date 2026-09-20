import { expect, test } from "@playwright/test";
import { createDashboard, openFineTune, shot, signIn } from "./helpers";

/**
 * The typography and content editor, driven the way an owner would drive it.
 *
 * The physical panel rejected the old type, so these specs check the three
 * things he asked for by name: change the words, change how they are set, and
 * take words off the panel without losing them.
 */

test.beforeEach(async ({ page }) => {
  await signIn(page);
});

/** The canvas the designer paints, as a data URL, for before/after compares. */
async function previewPixels(page: import("@playwright/test").Page): Promise<string> {
  return page.evaluate(() => {
    const canvas = document.querySelector<HTMLCanvasElement>(
      '[data-testid="designer-preview"]',
    );
    return canvas?.toDataURL() ?? "";
  });
}

test("every visible string on a module is editable and hideable", async ({
  page,
}, info) => {
  const { id } = await createDashboard(page, `Typography ${info.project.name}`);
  await page.goto(`/dashboards/${id}/edit`);

  await page.getByTestId("module-weather24h").click();
  await expect(page.getByTestId("module-inspector")).toBeVisible();

  // The heading is a role: wording, a shown switch, and a type panel.
  await expect(page.getByTestId("opt-heading-role")).toBeVisible();
  await expect(page.getByTestId("opt-heading")).toHaveValue("LES PROCHAINES 24H");
  await expect(page.getByTestId("opt-heading-visible")).toBeChecked();

  const before = await previewPixels(page);
  await page.getByTestId("opt-heading").fill("WEATHER · CÔTÉ JARDIN");
  await page.waitForTimeout(250);
  expect(await previewPixels(page)).not.toBe(before);

  // Hiding the heading takes the words off the panel and keeps them in the box.
  const withHeading = await previewPixels(page);
  await page.getByTestId("opt-heading-visible").uncheck();
  await page.waitForTimeout(250);
  expect(await previewPixels(page)).not.toBe(withHeading);
  await expect(page.getByTestId("opt-heading")).toHaveValue("WEATHER · CÔTÉ JARDIN");
});

test("the weather provenance line is off by default and can be turned on", async ({
  page,
}, info) => {
  const { id } = await createDashboard(page, `Provenance ${info.project.name}`);
  await page.goto(`/dashboards/${id}/edit`);

  await page.getByTestId("module-weather24h").click();
  // The line the owner rejected on the physical panel. Off unless they ask.
  await expect(page.getByTestId("opt-showProvenance")).not.toBeChecked();

  const before = await previewPixels(page);
  await page.getByTestId("opt-showProvenance").check();
  await page.waitForTimeout(250);
  expect(await previewPixels(page)).not.toBe(before);
});

test("font, size, weight and alignment each redraw the panel", async ({
  page,
}, info) => {
  const { id } = await createDashboard(page, `Type controls ${info.project.name}`);
  await page.goto(`/dashboards/${id}/edit`);

  await page.getByTestId("module-weather24h").click();
  await openFineTune(page);

  for (const [control, value] of [
    ["opt-heading-family", "atkinson"],
    ["opt-heading-size", "27"],
    ["opt-heading-weight", "regular"],
    ["opt-heading-align", "center"],
  ] as const) {
    const before = await previewPixels(page);
    await page.getByTestId(control).selectOption(value);
    await page.waitForTimeout(250);
    expect(await previewPixels(page), `${control} did not redraw`).not.toBe(before);
  }

  await shot(page, "inspector-typography", info.project.name);
});

test("every font family is offered and every size on the ladder", async ({
  page,
}, info) => {
  const { id } = await createDashboard(page, `Families ${info.project.name}`);
  await page.goto(`/dashboards/${id}/edit`);

  await page.getByTestId("module-weather24h").click();
  await openFineTune(page);

  const families = await page
    .getByTestId("opt-heading-family")
    .locator("option")
    .evaluateAll((options) => options.map((o) => (o as HTMLOptionElement).value));
  // Inheritance first, then the four vendored families in the order they were
  // chosen. There is no fifth entry, because there is no fifth atlas.
  expect(families).toEqual([
    "inherit",
    "inter",
    "atkinson",
    "plexmono",
    "poppins",
  ]);

  const sizes = await page
    .getByTestId("opt-heading-size")
    .locator("option")
    .evaluateAll((options) => options.map((o) => (o as HTMLOptionElement).value));
  // Exactly the sizes with a committed atlas. No free-text size box can
  // promise a size the renderer would then throw on.
  expect(sizes).toEqual(["11", "13", "15", "18", "22", "27", "34"]);

  // A specimen of each family at a small and a large size, for the QA sheet.
  for (const family of families) {
    for (const size of ["11", "34"]) {
      await page.getByTestId("opt-heading-family").selectOption(family);
      await page.getByTestId("opt-heading-size").selectOption(size);
      await page.waitForTimeout(200);
      await shot(page, `type-${family}-${size}`, info.project.name);
    }
  }
});

test("text that does not fit is warned about, not silently cut", async ({
  page,
}, info) => {
  const { id } = await createDashboard(page, `Overflow ${info.project.name}`);
  await page.goto(`/dashboards/${id}/edit`);

  // A clean starter says nothing, because its defaults fit.
  await expect(page.getByTestId("overflow-warnings")).toHaveCount(0);

  await page.getByTestId("module-message").click();
  await page
    .getByTestId("opt-body")
    .fill(
      "Ceci est un message beaucoup trop long pour tenir dans cette petite tuile du panneau",
    );
  await openFineTune(page);
  await page.getByTestId("opt-body-size").selectOption("27");

  const warning = page.getByTestId("overflow-warnings");
  await expect(warning).toBeVisible();
  await expect(warning).toContainText("does not fit");
  // The inspector points at the role, next to the field that caused it.
  await expect(page.getByTestId("opt-body-overflow")).toBeVisible();

  // It is a warning, not a block: the panel marks it in red and the push is
  // still the owner's decision to make.
  await expect(page.getByTestId("save-version")).toBeEnabled();
  await shot(page, "designer-overflow", info.project.name);

  // Shrinking it back clears the warning.
  await page.getByTestId("opt-body-size").selectOption("11");
  await page.getByTestId("opt-body").fill("Court");
  await expect(page.getByTestId("overflow-warnings")).toHaveCount(0);
});

test("a module can be hidden, shown again, and deleted", async ({ page }, info) => {
  const { id } = await createDashboard(page, `Hide ${info.project.name}`);
  await page.goto(`/dashboards/${id}/edit`);

  await page.getByTestId("module-weather24h").click();
  const shown = await previewPixels(page);

  await page.getByTestId("toggle-module-hidden").click();
  await expect(page.getByTestId("module-hidden-note")).toBeVisible();
  await expect(page.getByTestId("module-weather24h")).toHaveAttribute(
    "data-hidden",
    "true",
  );
  await page.waitForTimeout(250);
  const hidden = await previewPixels(page);
  expect(hidden).not.toBe(shown);

  // Hiding keeps the module, so it is still on the canvas and still editable.
  await expect(page.getByTestId("module-weather24h")).toHaveCount(1);
  await expect(page.getByTestId("opt-heading")).toHaveValue("LES PROCHAINES 24H");
  await shot(page, "designer-module-hidden", info.project.name);

  await page.getByTestId("toggle-module-hidden").click();
  await page.waitForTimeout(250);
  expect(await previewPixels(page)).toBe(shown);

  // Deleting is the other thing, and it says so on the button.
  await page.getByTestId("remove-module").click();
  await expect(page.getByTestId("module-weather24h")).toHaveCount(0);
  await page.waitForTimeout(250);
  expect(await previewPixels(page)).toBe(hidden);
});

test("a hidden module survives a save and a reload", async ({ page }, info) => {
  const { id } = await createDashboard(page, `Hide persist ${info.project.name}`);
  await page.goto(`/dashboards/${id}/edit`);

  await page.getByTestId("module-timestamp").click();
  await page.getByTestId("toggle-module-hidden").click();
  await page.getByTestId("save-version").click();
  await expect(page.getByTestId("version-rail")).toContainText("v2");

  await page.reload();
  await expect(page.getByTestId("module-timestamp")).toHaveAttribute(
    "data-hidden",
    "true",
  );
});

test("edited wording survives a save and a reload", async ({ page }, info) => {
  const { id } = await createDashboard(page, `Persist ${info.project.name}`);
  await page.goto(`/dashboards/${id}/edit`);

  await page.getByTestId("module-weather24h").click();
  await page.getByTestId("opt-heading").fill("MÉTÉO");
  await openFineTune(page);
  await page.getByTestId("opt-heading-family").selectOption("atkinson");
  await page.getByTestId("opt-heading-size").selectOption("22");

  await page.getByTestId("save-version").click();
  await expect(page.getByTestId("version-rail")).toContainText("v2");

  await page.reload();
  await page.getByTestId("module-weather24h").click();
  await expect(page.getByTestId("opt-heading")).toHaveValue("MÉTÉO");
  await openFineTune(page);
  await expect(page.getByTestId("opt-heading-family")).toHaveValue("atkinson");
  await expect(page.getByTestId("opt-heading-size")).toHaveValue("22");
});
