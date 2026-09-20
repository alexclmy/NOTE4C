import { expect, test } from "@playwright/test";
import { setMock, shot, signIn } from "./helpers";

/**
 * The Device page against an api 2 device.
 *
 * The mock speaks the real contract, so these are the tests that would catch
 * the Device page telling somebody a change had been made when it had not,
 * overwriting a change made on the device itself, or offering a control for a
 * capability the firmware never reported.
 */

test.beforeEach(async ({ page }) => {
  await signIn(page);
  await setMock(page, { reset: true, panelDelayMs: 50 });
});

test("negotiates api 2 and renders live controls only for reported capabilities", async ({
  page,
}, info) => {
  await page.goto("/device");

  await expect(page.getByTestId("config-supported")).toBeVisible();
  await expect(page.getByTestId("config-supported")).toContainText("api 2");

  // Typed controls, one per writable field.
  await expect(page.getByTestId("control-gallery.slide_min")).toBeVisible();
  await expect(page.getByTestId("control-system.sync_interval")).toBeVisible();
  await expect(page.getByTestId("control-voice.muted")).toBeVisible();
  await expect(page.getByTestId("control-network.lan_service")).toBeVisible();

  // Wi-Fi credentials are out of reach at every api level, by design.
  await expect(page.getByTestId("setting-network.wifi")).toHaveAttribute(
    "data-gated",
    "true",
  );
  await expect(page.getByTestId("control-network.wifi")).toHaveCount(0);

  // About comes from the device, not from a vendor string in the tower.
  const body = await page.content();
  expect(body).not.toContain("notellm");
  expect(body).not.toContain("Youn-Beta1.0");
  expect(body).not.toContain("lazyyoun");
  // The device's own name, from the device. On a phone the evidence cards are
  // folded, so open the one that holds it rather than asserting on a summary.
  const about = page.getByTestId("fold-about");
  if (await about.isVisible().catch(() => false)) {
    await about.locator("summary").click();
  }
  await expect(page.getByText("NOTE4C mock panel")).toBeVisible();

  await shot(page, "device-config-live", info.project.name);
});

test("batches edits into one Apply and labels how each took effect", async ({
  page,
}, info) => {
  await page.goto("/device");
  await expect(page.getByTestId("pending-count")).toHaveText("0");

  await page.getByTestId("control-gallery.slide_min").selectOption("30");
  await page.getByTestId("control-system.sync_interval").fill("0");
  await expect(page.getByTestId("pending-count")).toHaveText("2");

  await page.getByTestId("apply-config").click();
  await expect(page.getByTestId("apply-notice")).toContainText("confirmed 2 changes");
  await expect(page.getByTestId("pending-count")).toHaveText("0");

  // Each written field says what the device will actually do with it.
  await expect(page.getByText("takes effect now and survives a restart").first()).toBeVisible();

  // And the value survives a fresh read, so the page was not optimistic.
  await page.getByTestId("refresh").click();
  await expect(page.getByTestId("control-gallery.slide_min")).toHaveValue("30");
  await shot(page, "device-config-applied", info.project.name);
});

test("says plainly when a setting will not survive a restart", async ({ page }) => {
  await page.goto("/device");

  // The LAN service is runtime-only state on the device. Saying "immediate"
  // would let somebody believe it had stuck.
  const row = page.locator(".row", { hasText: "LAN service" }).first();
  await expect(row).toContainText("immediate, not saved");
});

test("asks for a typed word before severing its own connection", async ({ page }) => {
  await page.goto("/device");

  await page.getByTestId("control-network.lan_service").uncheck();
  await expect(page.getByTestId("batch-confirm")).toBeVisible();
  // Apply stays disabled until the exact word is typed.
  await expect(page.getByTestId("apply-config")).toBeDisabled();

  await page.getByTestId("batch-confirm").fill("yes");
  await expect(page.getByTestId("apply-config")).toBeDisabled();

  await page.getByTestId("batch-confirm").fill("LAN OFF");
  await expect(page.getByTestId("apply-config")).toBeEnabled();

  // Not clicked. The point of this test is the gate, and the mock would
  // faithfully report the API the rest of this suite talks through as off.
  await page.getByTestId("discard-config").click();
  await expect(page.getByTestId("pending-count")).toHaveText("0");
});

test("lockdown can be raised from here and never lowered", async ({ page }) => {
  await setMock(page, { reset: true });
  await page.goto("/device");

  // Lockdown defaults on, so the control that would turn it off is disabled
  // rather than offered and then refused.
  const control = page.getByTestId("control-dashboard.lockdown");
  await expect(control).toBeChecked();
  await expect(control).toBeDisabled();
  // And the reason is on the row, not only in a tooltip.
  await expect(page.getByTestId("oneway-dashboard.lockdown")).toContainText(
    "one way from here",
  );

  const row = page.locator(".row", { hasText: "Legacy writes blocked" }).first();
  await expect(row).toContainText("immediate");
});

test("a change made on the device is not overwritten by a stale edit", async ({
  page,
}, info) => {
  await page.goto("/device");
  await expect(page.getByTestId("config-revision")).toHaveText("0");

  // The user starts an edit.
  await page.getByTestId("control-gallery.slide_min").selectOption("30");
  await expect(page.getByTestId("pending-count")).toHaveText("1");

  // Somebody walks up to the device and changes the same setting.
  const state = await setMock(page, { bumpRevision: true, slideMin: 10 });
  expect(state.configRevision).toBe(1);

  await page.getByTestId("apply-config").click();

  // Refused, explained, and re-read. Never a blind overwrite.
  await expect(page.getByTestId("revision-conflict")).toBeVisible();
  await expect(page.getByTestId("revision-conflict")).toContainText(
    "Nothing was written",
  );
  await expect(page.getByTestId("revision-conflict")).toContainText("Review them");
  await expect(page.getByTestId("config-revision")).toHaveText("1");

  // The device's own value stands.
  const fresh = await page.request.get("/api/device/config");
  const body = (await fresh.json()) as {
    config: { gallery: { slide_min: number } };
  };
  expect(body.config.gallery.slide_min).toBe(10);

  await shot(page, "device-config-conflict", info.project.name);
});

test("restart and sleep are typed-confirmation gated and idempotent", async ({
  page,
}, info) => {
  await page.goto("/device");

  await page.getByTestId("action-system.restart").click();
  const dialog = page.getByRole("dialog");
  await expect(dialog).toContainText("answers first and reboots");
  await expect(page.getByTestId("confirm-ok")).toBeDisabled();

  await page.getByTestId("confirm-word").fill("RESTART");
  await expect(page.getByTestId("confirm-ok")).toBeEnabled();
  await page.getByTestId("confirm-ok").click();

  await expect(page.getByTestId("apply-notice")).toContainText("accepted it");
  await shot(page, "device-action-restart", info.project.name);

  // The device recorded exactly one restart, and never performed it: the mock
  // records rather than exits, which is what makes this assertable at all.
  const state = await setMock(page, {});
  expect(state.performedActions.filter((entry) => entry.action === "restart")).toHaveLength(1);

  // Sleep says what it costs: the tower cannot bring the device back.
  await page.getByTestId("action-system.sleep").click();
  await expect(page.getByRole("dialog")).toContainText("cannot reach it and cannot wake it");
  await page.getByTestId("confirm-cancel").click();
});

test("the Voice page reads the real mute state and writes it back", async ({
  page,
}, info) => {
  await page.goto("/voice");

  // Absent, not merely off: the mock does not advertise voice.ptt.v1.
  await expect(page.getByTestId("ptt-absent")).toBeVisible();

  await expect(page.getByTestId("voice-mute-state")).toHaveText("muted");
  // click(), not uncheck(): this control writes through to the device and the
  // box only moves once the device has confirmed. That lag is the product
  // behaviour, so the test waits for it rather than asserting it away.
  await page.getByTestId("voice-mute").click();
  await expect(page.getByTestId("voice-notice")).toContainText("confirmed the unmute");
  await expect(page.getByTestId("voice-notice")).toContainText("hub configured");
  await expect(page.getByTestId("voice-mute-state")).toHaveText("not muted");

  // The hub token stays write only even now that the config route exists.
  await expect(page.getByTestId("hub-url-value")).toContainText("not configured");
  await shot(page, "voice-config-live", info.project.name);
});
