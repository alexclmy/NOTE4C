import { expect, test, type Page } from "@playwright/test";
import {
  createEmptyDashboard,
  previewPixels,
  shot,
  signIn,
  stablePixels,
} from "./helpers";

/**
 * The three new modules, driven the way an owner would drive them.
 *
 * Every spec starts from an EMPTY dashboard rather than the starter, because
 * the starter fills the grid and a four by three List has nowhere to land on
 * it. That is correct product behaviour — the designer says so out loud — but
 * it is not what these specs are about.
 */

test.beforeEach(async ({ page }) => {
  await signIn(page);
});

async function openEmptyDesigner(page: Page, name: string): Promise<string> {
  const { id } = await createEmptyDashboard(page, name);
  await page.goto(`/dashboards/${id}/edit`);
  await expect(page.getByTestId("designer-stage")).toBeVisible();
  return id;
}

async function addRow(page: Page, index: number, text: string): Promise<void> {
  await page.getByTestId("opt-rows-add").click();
  await page.getByTestId(`opt-row-${index}-text`).fill(text);
}

test("a list is written, reordered, hidden and deleted row by row", async ({
  page,
}, info) => {
  await openEmptyDesigner(page, `List ${info.project.name}`);
  await page.getByTestId("add-list").click();
  await expect(page.getByTestId("module-list")).toBeVisible();

  // An empty list says so on the panel rather than drawing nothing.
  await expect(page.getByTestId("opt-rows-empty")).toBeVisible();
  await expect(page.getByTestId("opt-emptyText")).toHaveValue(
    "Rien pour l'instant",
  );
  const empty = await previewPixels(page);
  await shot(page, "list-empty", info.project.name);

  await addRow(page, 0, "Collect the parcel");
  await addRow(page, 1, "Take the bins out");
  await addRow(page, 2, "Water the plants");
  await page.waitForTimeout(250);
  const three = await previewPixels(page);
  expect(three).not.toBe(empty);

  // Reordering moves a row, and the panel follows.
  await page.getByTestId("opt-row-2-up").click();
  await page.waitForTimeout(250);
  const reordered = await previewPixels(page);
  expect(reordered).not.toBe(three);
  await expect(page.getByTestId("opt-row-1-text")).toHaveValue("Water the plants");
  await expect(page.getByTestId("opt-row-2-text")).toHaveValue("Take the bins out");

  // The first row cannot move up, and the last cannot move down.
  await expect(page.getByTestId("opt-row-0-up")).toBeDisabled();
  await expect(page.getByTestId("opt-row-2-down")).toBeDisabled();

  // Hiding a row takes it off the panel and keeps its wording.
  await page.getByTestId("opt-row-0-visible").uncheck();
  await page.waitForTimeout(250);
  expect(await previewPixels(page)).not.toBe(reordered);
  await expect(page.getByTestId("opt-row-0-text")).toHaveValue(
    "Collect the parcel",
  );

  // Deleting is the other thing, and it says so on the button.
  await page.getByTestId("opt-row-0-remove").click();
  await expect(page.getByTestId("opt-row-2-text")).toHaveCount(0);
  await expect(page.getByTestId("opt-row-0-text")).toHaveValue("Water the plants");
  await shot(page, "list-configured", info.project.name);
});

test("a list marks the rows it is not showing instead of dropping them", async ({
  page,
}, info) => {
  await openEmptyDesigner(page, `List overflow ${info.project.name}`);
  await page.getByTestId("add-list").click();

  await expect(page.getByTestId("overflow-warnings")).toHaveCount(0);
  for (let index = 0; index < 8; index += 1) {
    await addRow(page, index, `Rangée numéro ${index + 1}`);
  }
  await page.getByTestId("opt-maxVisibleRows").fill("4");
  await page.waitForTimeout(250);

  const warning = page.getByTestId("overflow-warnings");
  await expect(warning).toBeVisible();
  await expect(page.getByTestId("opt-rows-overflow")).toBeVisible();
  // A warning, not a block.
  await expect(page.getByTestId("save-version")).toBeEnabled();
  await shot(page, "list-overflow", info.project.name);

  // Deleting the rows it could not show clears it. Raising the maximum would
  // not have: eight lines do not fit a four by three tile either, and the
  // warning would rightly have carried on saying so.
  for (let index = 0; index < 4; index += 1) {
    await page.getByTestId("opt-row-4-remove").click();
  }
  await page.waitForTimeout(250);
  await expect(page.getByTestId("overflow-warnings")).toHaveCount(0);
});

test("the list marker modes each redraw, and the checkbox says it is a drawing", async ({
  page,
}, info) => {
  await openEmptyDesigner(page, `List markers ${info.project.name}`);
  await page.getByTestId("add-list").click();
  await addRow(page, 0, "Collect the parcel");
  await page.waitForTimeout(250);

  const seen = new Set<string>();
  for (const marker of ["bullet", "numbered", "checkbox"]) {
    await page.getByTestId("opt-marker").selectOption(marker);
    await page.waitForTimeout(250);
    seen.add(await previewPixels(page));
  }
  expect(seen.size).toBe(3);

  // The panel has no buttons, and the inspector does not pretend otherwise.
  await expect(page.getByTestId("module-inspector")).toContainText(
    "nothing on it can be ticked",
  );
});

test("a list survives a save and a reload, rows, order and all", async ({
  page,
}, info) => {
  const id = await openEmptyDesigner(page, `List persist ${info.project.name}`);
  await page.getByTestId("add-list").click();
  await addRow(page, 0, "Premier");
  await addRow(page, 1, "Deuxième");
  await page.getByTestId("opt-row-1-visible").uncheck();
  await page.getByTestId("opt-marker").selectOption("numbered");
  await page.waitForTimeout(250);
  const before = await previewPixels(page);

  await page.getByTestId("save-version").click();
  await expect(page.getByTestId("version-rail")).toContainText("v2");

  await page.goto(`/dashboards/${id}/edit`);
  await page.getByTestId("module-list").click();
  await expect(page.getByTestId("opt-row-0-text")).toHaveValue("Premier");
  await expect(page.getByTestId("opt-row-1-text")).toHaveValue("Deuxième");
  await expect(page.getByTestId("opt-row-1-visible")).not.toBeChecked();
  await expect(page.getByTestId("opt-marker")).toHaveValue("numbered");
  expect(await stablePixels(page)).toBe(before);
});

test("a countdown asks for a date instead of counting to zero", async ({
  page,
}, info) => {
  await openEmptyDesigner(page, `Countdown ${info.project.name}`);
  await page.getByTestId("add-countdown").click();
  await expect(page.getByTestId("module-countdown")).toBeVisible();

  // No target: an explicit configuration state, and the field is empty.
  await expect(page.getByTestId("opt-targetAt")).toHaveValue("");
  await expect(page.getByTestId("opt-unavailableTitle")).toHaveValue(
    "Compte à rebours",
  );
  const unset = await previewPixels(page);
  await shot(page, "countdown-unavailable", info.project.name);

  await page.getByTestId("opt-targetAt").fill("2027-12-25T08:00");
  await page.waitForTimeout(300);
  const counting = await previewPixels(page);
  expect(counting).not.toBe(unset);
  await shot(page, "countdown-configured", info.project.name);

  // The unit is a choice, and each one is a different panel.
  await page.getByTestId("opt-unit").selectOption("hours");
  await page.waitForTimeout(300);
  expect(await previewPixels(page)).not.toBe(counting);

  // Clearing the date goes back to asking, not to zero.
  await page.getByTestId("opt-unit").selectOption("auto");
  await page.getByTestId("opt-targetAt").fill("");
  await page.waitForTimeout(300);
  expect(await previewPixels(page)).toBe(unset);
});

test("a countdown refuses a timezone this machine does not know", async ({
  page,
}, info) => {
  await openEmptyDesigner(page, `Countdown tz ${info.project.name}`);
  await page.getByTestId("add-countdown").click();
  await page.getByTestId("opt-targetAt").fill("2027-12-25T08:00");

  await page.getByTestId("opt-timeZone").fill("Mars/Olympus");
  await expect(page.getByTestId("layout-problems")).toContainText("IANA");
  // A layout the renderer would reject cannot be saved.
  await expect(page.getByTestId("save-version")).toBeDisabled();

  await page.getByTestId("opt-timeZone").fill("Europe/Paris");
  await expect(page.getByTestId("layout-problems")).toHaveCount(0);
  await expect(page.getByTestId("save-version")).toBeEnabled();
});

test("a conditional message will not show itself until it has a condition", async ({
  page,
}, info) => {
  await openEmptyDesigner(page, `Conditional ${info.project.name}`);
  await page.getByTestId("add-conditionalMessage").click();
  await expect(page.getByTestId("module-conditionalMessage")).toBeVisible();

  await expect(page.getByTestId("opt-conditions-empty")).toBeVisible();
  await page.getByTestId("opt-body").fill("Take the bins out ce soir");
  await page.waitForTimeout(250);
  const unconfigured = await previewPixels(page);
  await shot(page, "conditional-unconfigured", info.project.name);

  // The message is typed in and the panel still refuses to show it.
  await expect(page.getByTestId("module-inspector")).toContainText(
    "would just be a message",
  );

  await page.getByTestId("opt-conditions-add").click();
  await expect(page.getByTestId("opt-condition-0-kind")).toHaveValue(
    "daysOfWeek",
  );
  // Nothing chosen yet is still not configured.
  await expect(page.getByTestId("opt-condition-0-explain")).toContainText(
    "none chosen yet",
  );
  await page.waitForTimeout(250);
  expect(await previewPixels(page)).toBe(unconfigured);

  // Every day, so the spec does not depend on which day it runs.
  for (const day of [0, 1, 2, 3, 4, 5, 6]) {
    await page.getByTestId(`opt-condition-0-day-${day}`).check();
  }
  await page.waitForTimeout(300);
  expect(await previewPixels(page)).not.toBe(unconfigured);
  await expect(page.getByTestId("opt-condition-0-explain")).toContainText(
    "Sunday, Monday",
  );
  await shot(page, "conditional-configured", info.project.name);
});

test("every condition kind is offered, explains itself, and none is code", async ({
  page,
}, info) => {
  await openEmptyDesigner(page, `Conditions ${info.project.name}`);
  await page.getByTestId("add-conditionalMessage").click();
  await page.getByTestId("opt-conditions-add").click();

  const kinds = await page
    .getByTestId("opt-condition-0-kind")
    .locator("option")
    .evaluateAll((options) => options.map((o) => (o as HTMLOptionElement).value));
  expect(kinds).toEqual([
    "dateRange",
    "daysOfWeek",
    "sourceState",
    "remindersCount",
  ]);

  for (const kind of kinds) {
    await page.getByTestId("opt-condition-0-kind").selectOption(kind);
    await expect(page.getByTestId("opt-condition-0-explain")).not.toBeEmpty();
  }

  // A reminders condition reads a count and says where it comes from.
  await page.getByTestId("opt-condition-0-kind").selectOption("remindersCount");
  await page.getByTestId("opt-condition-0-value").fill("2");
  await expect(page.getByTestId("opt-condition-0-explain")).toContainText(
    "2 or more open",
  );
  await expect(page.getByTestId("module-inspector")).toContainText(
    "their words stay on the Mac",
  );

  // And the whole editor says what this module is not.
  await expect(page.getByTestId("opt-conditions-role")).toContainText(
    "This is not an alarm",
  );
});

test("a conditional message survives a save and a reload", async ({
  page,
}, info) => {
  const id = await openEmptyDesigner(
    page,
    `Conditional persist ${info.project.name}`,
  );
  await page.getByTestId("add-conditionalMessage").click();
  await page.getByTestId("opt-body").fill("Récolte des pommes");
  await page.getByTestId("opt-conditions-add").click();
  await page.getByTestId("opt-condition-0-kind").selectOption("dateRange");
  await page.getByTestId("opt-condition-0-from").fill("2026-09-01");
  await page.getByTestId("opt-condition-0-to").fill("2026-12-31");
  await page.waitForTimeout(250);
  const before = await previewPixels(page);

  await page.getByTestId("save-version").click();
  await expect(page.getByTestId("version-rail")).toContainText("v2");

  await page.goto(`/dashboards/${id}/edit`);
  await page.getByTestId("module-conditionalMessage").click();
  await expect(page.getByTestId("opt-body")).toHaveValue("Récolte des pommes");
  await expect(page.getByTestId("opt-condition-0-from")).toHaveValue(
    "2026-09-01",
  );
  await expect(page.getByTestId("opt-condition-0-to")).toHaveValue("2026-12-31");
  expect(await stablePixels(page)).toBe(before);
});

test("the row and condition controls are reachable on a phone", async ({
  page,
}, info) => {
  test.skip(info.project.name !== "mobile-chromium", "Mobile layout only");

  await openEmptyDesigner(page, `Mobile rows ${info.project.name}`);
  await page.getByTestId("add-list").click();
  await addRow(page, 0, "Collect the parcel");
  await addRow(page, 1, "Take the bins out");

  for (const testId of [
    "opt-row-0-text",
    "opt-row-0-visible",
    "opt-row-0-down",
    "opt-row-0-remove",
    "opt-rows-add",
  ]) {
    const box = await page.getByTestId(testId).boundingBox();
    expect(box, `${testId} has no box`).not.toBeNull();
    if (!box) continue;
    // Inside the 390 px viewport, and not a one-pixel target.
    expect(box.x).toBeGreaterThanOrEqual(0);
    expect(box.x + box.width).toBeLessThanOrEqual(391);
    expect(box.height).toBeGreaterThanOrEqual(24);
  }

  // And the control actually works under a tap rather than a click.
  await page.getByTestId("opt-row-0-down").tap();
  await expect(page.getByTestId("opt-row-0-text")).toHaveValue("Take the bins out");
  await shot(page, "mobile-list-editor", info.project.name);
});
