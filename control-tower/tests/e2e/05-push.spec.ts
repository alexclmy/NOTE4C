import { expect, test } from "@playwright/test";
import { createDistinctDashboard, selectDashboard, shot, signIn } from "./helpers";

test.beforeEach(async ({ page }) => {
  await signIn(page);
});

test("pushes to the mock device and reaches DISPLAYED", async ({ page }, info) => {
  const { id, title, version } = await createDistinctDashboard(
    page,
    `Push ${info.project.name}`,
  );
  await selectDashboard(page, id);

  await page.goto("/overview");
  await expect(page.getByTestId("push-button")).toBeEnabled();
  await shot(page, "overview-before-push", info.project.name);

  await page.getByTestId("push-button").click();

  // The review: the exact frame, rendered by a dry run through the same
  // pipeline the send uses. Nothing has reached the device yet.
  await expect(page.getByTestId("send-review")).toBeVisible();
  await expect(page.getByTestId("send-frame")).toBeVisible();
  // Mock mode gets a plain confirm; the typed word is only for the real device.
  await expect(page.getByTestId("send-confirm-word")).toHaveCount(0);
  await page.getByTestId("send-confirm").click();

  // The verdict, and only for the one outcome that means the device said so.
  // "Displayed" is never shown for anything else — an uncertain send gets its
  // own screen, because the honest word for "we sent it and never heard back"
  // is not "done" and not "failed".
  const done = page.getByTestId("send-done");
  await expect(done).toBeVisible({ timeout: 45_000 });
  await expect(done).toContainText("confirmed it");
  await page.getByTestId("send-close-done").click();

  // The device itself now reports the frame as displayed.
  // Overview waits on a live Open-Meteo read before it renders, so give the
  // first paint after a reload room for a slow external source.
  await expect(page.locator('[data-badge="displayed"]').first()).toBeVisible({
    timeout: 40_000,
  });
  await expect(page.getByText(`${title} v${version}`).first()).toBeVisible();
  await shot(page, "overview-after-push", info.project.name);

  // And the library card carries the DISPLAYED badge, resolved through the
  // ledger's digest map rather than guessed.
  await page.goto("/dashboards");
  await expect(
    page.getByTestId(`dash-${id}`).locator('[data-badge="displayed"]'),
  ).toBeVisible();
});

test("an unchanged dashboard reports that it would dedup", async ({ page }, info) => {
  const { id } = await createDistinctDashboard(page, `Dedup ${info.project.name}`);
  await selectDashboard(page, id);

  await page.goto("/overview");
  await page.getByTestId("push-button").click();
  await page.getByTestId("send-confirm").click();
  await expect(page.getByTestId("send-done")).toBeVisible({ timeout: 45_000 });
  await page.getByTestId("send-close-done").click();

  await page.reload();
  await expect(page.getByTestId("would-dedup")).toBeVisible();

  // Sending anyway is allowed, and the review says plainly what it would cost
  // before anything is pressed — rather than letting somebody confirm and then
  // explaining on the way out that nothing happened.
  await page.getByTestId("push-button").click();
  await expect(page.getByTestId("send-would-dedup")).toContainText(
    /already shows exactly this/i,
  );
  await expect(page.getByTestId("send-would-dedup")).toContainText(/spend battery/i);
  await page.getByTestId("send-cancel").click();
  await expect(page.getByTestId("send-flow")).toHaveCount(0);
});

test("the ledger and audit log record the push honestly", async ({ page }, info) => {
  const { id, title } = await createDistinctDashboard(
    page,
    `Ledger ${info.project.name}`,
  );
  await selectDashboard(page, id);

  await page.goto("/overview");
  await page.getByTestId("push-button").click();
  await page.getByTestId("send-confirm").click();
  await expect(page.getByTestId("send-done")).toBeVisible({ timeout: 45_000 });
  await page.getByTestId("send-close-done").click();

  await page.goto("/diagnostics");
  // Send history, which is one of the four tabs now rather than a card in a
  // stack of nine. The testids moved with the content they name.
  await page.getByTestId("diag-tab-history").click();
  const ledger = page.getByTestId("ledger-table");
  await expect(ledger).toBeVisible();
  await expect(ledger).toContainText(title);
  await expect(ledger.locator('[data-badge="displayed"]').first()).toBeVisible();

  await page.getByTestId("diag-tab-events").click();
  const audit = page.getByTestId("audit-table");
  await expect(audit).toContainText("device.push");
  // Whether the device confirmed is the whole point of the audit log, so it is
  // on the line rather than in a column somebody has to scroll sideways to.
  await expect(
    audit.locator(".listing-row", { hasText: "device.push" }).first(),
  ).toContainText("device confirmed: yes");
  await shot(page, "diagnostics-after-push", info.project.name);
});

test("no secret ever reaches the browser", async ({ page }) => {
  await page.goto("/diagnostics");
  await page.getByTestId("diag-tab-history").click();
  await expect(page.getByTestId("ledger-table")).toBeVisible();

  const body = (await page.content()).toLowerCase();
  // The mock's dev token, the shape of a real device token, and the words that
  // would carry one.
  expect(body).not.toContain("0".repeat(64));
  expect(body).not.toMatch(/"token"\s*:\s*"[0-9a-f]{64}"/);

  const state = await page.request.get("/api/state");
  const json = await state.json();
  expect(Object.keys(json)).toContain("deviceTokenSet");
  expect(JSON.stringify(json)).not.toMatch(/[0-9a-f]{64}/);
});

test("the Voice hub token field is write only and never echoes", async ({
  page,
}, info) => {
  await page.goto("/voice");
  const tokenField = page.getByTestId("hub-token");

  await expect(tokenField).toHaveValue("");
  await expect(tokenField).toHaveAttribute("type", "password");

  await page.getByTestId("hub-url").fill("http://192.168.7.7:8770");
  await tokenField.fill("hub-secret-value-do-not-echo");
  await page.getByTestId("save-hub").click();

  await expect(page.getByTestId("hub-result")).toBeVisible();
  await expect(page.getByTestId("hub-result")).toContainText("configured");
  // Cleared after the write, and nowhere in the document either.
  await expect(tokenField).toHaveValue("");
  expect(await page.content()).not.toContain("hub-secret-value-do-not-echo");

  await expect(page.getByTestId("plaintext-warning")).toBeVisible();
  await shot(page, "voice-after-hub-write", info.project.name);
});

test("wake word copy states plainly that nothing listens", async ({ page }) => {
  await page.goto("/voice");
  await expect(page.getByTestId("wake-word-copy")).toContainText(
    "No ambient listening exists or is claimed",
  );
});
