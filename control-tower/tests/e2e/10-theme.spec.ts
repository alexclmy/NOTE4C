import { expect, test, type Page } from "@playwright/test";
import {
  closeInspectorSheet,
  createDashboard,
  openFineTune,
  openThemeAdvanced,
  previewPixels,
  shot,
  signIn,
  stablePixels,
} from "./helpers";

/**
 * The dashboard theme: padding, the dashboard font, and the palette policy.
 *
 * The thing these specs are really pinning is that none of it happens to the owner
 * by accident. Choosing a font changes nothing until he applies it, turning a
 * pigment off explains what will be drawn instead, and every one of these is a
 * version he can roll back.
 */

test.beforeEach(async ({ page }) => {
  await signIn(page);
});

test("content padding moves the layout and says what it costs", async ({
  page,
}, info) => {
  const { id } = await createDashboard(page, `Padding ${info.project.name}`);
  await page.goto(`/dashboards/${id}/edit`);
  // Padding, the dashboard font and the palette live behind the Theme panel's
  // one "Advanced" fold now; the Appearance pickers stay on the first surface.
  await openThemeAdvanced(page);
  await expect(page.getByTestId("theme-padding-readout")).toContainText(
    "0 px a side",
  );
  await expect(page.getByTestId("theme-padding-readout")).toContainText(
    "400 by 300",
  );
  const flush = await stablePixels(page);

  await page.getByTestId("theme-padding").fill("16");
  await page.waitForTimeout(300);
  expect(await previewPixels(page)).not.toBe(flush);
  await expect(page.getByTestId("theme-padding-readout")).toContainText(
    "368 by 268",
  );
  await shot(page, "theme-padding", info.project.name);

  // The grid contract survives: still eight by six, and the modules moved with
  // it rather than being cropped by it.
  const stage = await page.getByTestId("designer-stage").boundingBox();
  const tile = await page.getByTestId("module-weather24h").boundingBox();
  expect(stage).not.toBeNull();
  expect(tile).not.toBeNull();
  if (stage && tile) {
    expect(tile.x).toBeGreaterThan(stage.x);
    expect(tile.y).toBeGreaterThan(stage.y);
  }

  // The most padding the schema allows is still a legal, saveable dashboard.
  await page.getByTestId("theme-padding").fill("24");
  await page.waitForTimeout(300);
  await expect(page.getByTestId("layout-problems")).toHaveCount(0);
  await expect(page.getByTestId("save-version")).toBeEnabled();
});

test("padding survives a save and a reload", async ({ page }, info) => {
  const { id } = await createDashboard(page, `Padding keep ${info.project.name}`);
  await page.goto(`/dashboards/${id}/edit`);
  await openThemeAdvanced(page);
  await page.getByTestId("theme-padding").fill("12");
  // A stable capture, not a single shot: a theme change now also repaints the
  // component gallery's thumbnails, so the main preview can still be settling a
  // frame later. stablePixels waits for two identical frames, which is what the
  // reload side compares against anyway.
  const padded = await stablePixels(page);

  await page.getByTestId("save-version").click();
  await expect(page.getByTestId("version-rail")).toContainText("v2");

  await page.goto(`/dashboards/${id}/edit`);
  await expect(page.getByTestId("theme-padding")).toHaveValue("12");
  expect(await stablePixels(page)).toBe(padded);
});

test("the dashboard font changes nothing until it is applied", async ({
  page,
}, info) => {
  const { id } = await createDashboard(page, `Global font ${info.project.name}`);
  await page.goto(`/dashboards/${id}/edit`);
  await openThemeAdvanced(page);

  await expect(page.getByTestId("theme-inheritance")).toContainText(
    "0 of",
  );
  const before = await stablePixels(page);

  // Picking a font is not a rewrite.
  await page.getByTestId("theme-family").selectOption("poppins");
  await page.waitForTimeout(300);
  expect(await previewPixels(page)).toBe(before);
  await shot(page, "theme-font-picked", info.project.name);

  // Applying is, and it asks first, with the panel it would produce.
  await page.getByTestId("theme-apply-all").click();
  await expect(page.getByTestId("theme-apply-preview")).toBeVisible();
  await expect(page.getByTestId("theme-preview-canvas")).toBeVisible();
  await shot(page, "theme-apply-confirm", info.project.name);

  await page.getByTestId("confirm-cancel").click();
  await page.waitForTimeout(250);
  expect(await previewPixels(page)).toBe(before);

  await page.getByTestId("theme-apply-all").click();
  await page.getByTestId("confirm-ok").click();
  await page.waitForTimeout(300);
  const applied = await previewPixels(page);
  expect(applied).not.toBe(before);
  await expect(page.getByTestId("theme-inheritance")).toContainText(
    "0 keep their own",
  );

  // A role now says where its face comes from rather than naming one.
  await page.getByTestId("module-weather24h").click();
  await openFineTune(page);
  await expect(page.getByTestId("opt-heading-family")).toHaveValue("inherit");

  // And changing the dashboard font now moves everything at once.
  await page.getByTestId("theme-family").selectOption("atkinson");
  await page.waitForTimeout(300);
  expect(await previewPixels(page)).not.toBe(applied);
});

test("applying the dashboard font is one version, so it rolls back", async ({
  page,
}, info) => {
  const { id } = await createDashboard(page, `Font rollback ${info.project.name}`);
  await page.goto(`/dashboards/${id}/edit`);
  await openThemeAdvanced(page);
  const original = await stablePixels(page);

  // Version 1 is the one createDashboard saved, so there is already something
  // to roll back to.
  await page.getByTestId("theme-family").selectOption("poppins");
  await page.getByTestId("theme-apply-all").click();
  await page.getByTestId("confirm-ok").click();
  await page.getByTestId("save-version").click();
  await expect(page.getByTestId("version-rail")).toContainText("v2");
  await page.waitForTimeout(250);
  expect(await previewPixels(page)).not.toBe(original);

  // The version history is a popover on the title line now, rather than a card
  // in the sidebar. Opening it is the only added step: every assertion below —
  // including every pixel comparison — is unchanged.
  await page.getByTestId("version-toggle").click();
  await page.getByTestId("rollback-1").click();
  await expect(page.getByTestId("version-rail")).toContainText("from v1");
  expect(await stablePixels(page)).toBe(original);
});

test("turning a pigment off explains itself instead of hiding it", async ({
  page,
}, info) => {
  const { id } = await createDashboard(page, `Palette ${info.project.name}`);
  await page.goto(`/dashboards/${id}/edit`);
  await openThemeAdvanced(page);

  // Black and white are structural and the UI says why rather than hiding them.
  await expect(page.getByTestId("theme-palette-black")).toBeDisabled();
  await expect(page.getByTestId("theme-palette-black")).toBeChecked();
  await expect(page.getByTestId("theme-palette-white")).toBeDisabled();

  const full = await stablePixels(page);
  await page.getByTestId("theme-palette-red").uncheck();
  await page.waitForTimeout(300);

  const explanation = page.getByTestId("theme-palette-fallback");
  await expect(explanation).toBeVisible();
  await expect(explanation).toContainText("still physically there");
  await expect(page.getByTestId("palette-remap-note")).toBeVisible();
  expect(await previewPixels(page)).not.toBe(full);
  await shot(page, "theme-palette-red-off", info.project.name);

  await page.getByTestId("theme-palette-yellow").uncheck();
  await page.waitForTimeout(300);
  await expect(explanation).toContainText("drawn in black");

  // Putting them back puts the panel back.
  await page.getByTestId("theme-palette-red").check();
  await page.getByTestId("theme-palette-yellow").check();
  await page.waitForTimeout(300);
  expect(await previewPixels(page)).toBe(full);
  await expect(page.getByTestId("theme-palette-fallback")).toHaveCount(0);
});

test("a per-element colour is drawn, and an unreadable one is called out", async ({
  page,
}, info) => {
  const { id } = await createDashboard(page, `Element colour ${info.project.name}`);
  await page.goto(`/dashboards/${id}/edit`);

  await page.getByTestId("module-message").click();
  await page.getByTestId("opt-body").fill("Collect the parcel");
  await openFineTune(page);
  const ink = await stablePixels(page);

  await page.getByTestId("opt-body-colour-accent").click();
  await page.waitForTimeout(300);
  const accented = await previewPixels(page);
  expect(accented).not.toBe(ink);
  await shot(page, "theme-element-colour", info.project.name);

  // White on the paper is a real choice and a bad one. It is drawn, and said.
  await page.getByTestId("opt-body-colour-paper").click();
  await page.waitForTimeout(300);
  await expect(page.getByTestId("contrast-warnings")).toBeVisible();
  await expect(page.getByTestId("opt-body-contrast")).toContainText(
    "cannot be read",
  );
  await expect(page.getByTestId("save-version")).toBeEnabled();
  await shot(page, "theme-contrast-warning", info.project.name);

  // Yellow is readable-ish and gets the softer sentence.
  await page.getByTestId("opt-body-colour-highlight").click();
  await page.waitForTimeout(300);
  await expect(page.getByTestId("opt-body-contrast")).toContainText("pale");
});

test("theme and element colours survive a save, a reload and a rollback", async ({
  page,
}, info) => {
  const { id } = await createDashboard(page, `Theme persist ${info.project.name}`);
  await page.goto(`/dashboards/${id}/edit`);
  await openThemeAdvanced(page);

  await page.getByTestId("theme-padding").fill("8");
  await page.getByTestId("theme-palette-yellow").uncheck();
  await page.getByTestId("module-message").click();
  await page.getByTestId("opt-body").fill("Parcel");
  await openFineTune(page);
  await page.getByTestId("opt-body-colour-accent").click();
  const themed = await stablePixels(page);

  await page.getByTestId("save-version").click();
  await expect(page.getByTestId("version-rail")).toContainText("v2");

  await page.goto(`/dashboards/${id}/edit`);
  await expect(page.getByTestId("theme-padding")).toHaveValue("8");
  await expect(page.getByTestId("theme-palette-yellow")).not.toBeChecked();
  await page.getByTestId("module-message").click();
  await openFineTune(page);
  // The five colour tokens are swatches of the pigment each one resolves to,
  // rather than a dropdown: these are fixed choices whose whole meaning is
  // visual, and a select that has to be opened to see the colours hides its
  // own options. The stored value is still the token, which is what makes it
  // survive a theme change — and what `aria-pressed` reports here.
  await expect(page.getByTestId("opt-body-colour-accent")).toHaveAttribute(
    "aria-pressed",
    "true",
  );
  expect(await stablePixels(page)).toBe(themed);

  // Version 1 predates all of it and comes back untouched. On a phone the
  // inspector sheet is over the version rail, so put it away first.
  await closeInspectorSheet(page);
  // The version history is a popover on the title line now, rather than a card
  // in the sidebar. Opening it is the only added step: every assertion below —
  // including every pixel comparison — is unchanged.
  await page.getByTestId("version-toggle").click();
  await page.getByTestId("rollback-1").click();
  await expect(page.getByTestId("version-rail")).toContainText("from v1");
  await page.waitForTimeout(300);
  await expect(page.getByTestId("theme-padding")).toHaveValue("0");
  await expect(page.getByTestId("theme-palette-yellow")).toBeChecked();
});
