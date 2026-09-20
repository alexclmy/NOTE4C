import { expect, test } from "@playwright/test";
import { createDashboard, signIn } from "./helpers";

test.beforeEach(async ({ page }) => {
  await signIn(page);
});

test("creates, previews, duplicates, archives and restores a composition", async ({
  page,
}, info) => {
  const { id, title } = await createDashboard(page, `CRUD ${info.project.name}`);
  expect(id).toMatch(/^d_/);

  const card = page.getByTestId(`dash-${id}`);
  await expect(card).toContainText("6 modules");
  await expect(card).toContainText("v1");

  // The preview is a real server render, so it must actually be a PNG. It is
  // also the button: the picture is what a composition is recognised by, so it
  // is the thing a person presses to open it.
  const preview = card.locator("img");
  await expect(preview).toBeVisible();
  const response = await page.request.get(`/api/dashboards/${id}/preview`);
  expect(response.status()).toBe(200);
  expect(response.headers()["content-type"]).toBe("image/png");
  expect((await response.body()).byteLength).toBeGreaterThan(200);

  // Show on panel and Edit stay on the card; the occasional actions live
  // behind the "..." toggle, which opens a row under the card's own rule
  // rather than a floating panel over a list that is about to reload.
  await card.getByTestId(`more-${id}`).click();
  // Scoped to the row: the toggle's own label names the composition, so one
  // called "Archive something" would match a bare name lookup too.
  await page.getByTestId(`more-panel-${id}`).getByRole("button", { name: "Duplicate" }).click();
  await expect(
    page.locator('[data-testid^="dash-"]').filter({ hasText: `${title} copy` }),
  ).toBeVisible();

  await card.getByTestId(`more-${id}`).click();
  await page.getByTestId(`more-panel-${id}`).getByRole("button", { name: "Archive" }).click();
  // Gone from the gallery grid, and the toast says nothing on the panel moved.
  await expect(page.getByTestId("dashboard-grid").getByTestId(`dash-${id}`)).toHaveCount(0);
  await expect(page.getByTestId("toast")).toContainText("nothing on the panel changed");

  // Archived compositions are folded away rather than fetched on demand, so
  // the count is known before anything is expanded.
  const fold = page.getByTestId("archived-fold");
  await expect(fold).toContainText("Archived (1)");
  await page.getByTestId("show-archived").click();
  await expect(page.getByTestId(`restore-${id}`)).toBeVisible();

  await page.getByTestId(`restore-${id}`).click();
  await expect(
    page.getByTestId("dashboard-grid").getByTestId(`dash-${id}`),
  ).toBeVisible();
});

test("the thumbnail opens the editor", async ({ page }, info) => {
  const { id, title } = await createDashboard(page, `Open ${info.project.name}`);

  await page.getByTestId(`open-${id}`).click();
  await page.waitForURL(`**/dashboards/${id}/edit`);
  await expect(page.getByTestId("designer-title")).toHaveText(title);
});

test("Show on panel selects the composition and opens the review", async ({
  page,
}, info) => {
  const { id, title } = await createDashboard(page, `Select ${info.project.name}`);

  await page.getByTestId(`select-${id}`).click();

  // The review, showing the exact frame that would be sent. Nothing has
  // reached the device: this is a dry run through the same pipeline.
  await expect(page.getByTestId("send-review")).toBeVisible();
  await expect(page.getByTestId("send-review")).toContainText(
    "This exact image will be sent to the panel",
  );
  await page.getByTestId("send-cancel").click();
  await expect(page.getByTestId("send-flow")).toHaveCount(0);

  // Cancelling the send does not undo the selection: choosing what the panel
  // shows next is what pressing that button means, and it is reversible by
  // choosing another.
  await expect(page.getByTestId(`dash-${id}`).locator('[data-badge="selected"]')).toBeVisible();

  await page.goto("/overview");
  await expect(page.getByText(title)).toBeVisible();
  await expect(page.getByTestId("push-button")).toBeEnabled();
});

test("archiving the selected composition clears the selection", async ({ page }, info) => {
  const { id } = await createDashboard(page, `Archive selected ${info.project.name}`);

  await page.getByTestId(`select-${id}`).click();
  await page.getByTestId("send-cancel").click();
  await expect(page.getByTestId(`dash-${id}`).locator('[data-badge="selected"]')).toBeVisible();

  await page.getByTestId(`dash-${id}`).getByTestId(`more-${id}`).click();
  await page.getByTestId(`more-panel-${id}`).getByRole("button", { name: "Archive" }).click();
  await expect(page.getByTestId("dashboard-grid").getByTestId(`dash-${id}`)).toHaveCount(0);

  await page.goto("/overview");
  // A composition that cannot be edited must not be the thing the next send
  // reaches for.
  await expect(page.getByTestId("push-button")).toBeDisabled();
  await expect(page.getByTestId("no-selection")).toContainText(
    "No composition is selected",
  );
});
