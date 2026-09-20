import { expect, test } from "@playwright/test";
import { setMock, shot, signIn } from "./helpers";

/**
 * Unsaved work follows you down the page.
 *
 * The defect: Apply lived in its own card after five cards of settings. A
 * field marked dirty in yellow could be a screen and a half above the button
 * that would write it, and the count of pending changes was invisible while
 * you were making them.
 */

test.beforeEach(async ({ page }) => {
  await signIn(page);
  await setMock(page, { reset: true, panelDelayMs: 50 });
});

test("editing a field raises a bar with the count, and Apply works from it", async ({
  page,
}, info) => {
  await page.goto("/device");

  // Nothing pending: no bar at all. A bar that is always there is chrome.
  await expect(page.getByTestId("sticky-actions")).toHaveCount(0);
  await expect(page.getByTestId("pending-count")).toHaveText("0");

  await page.getByTestId("control-gallery.slide_min").selectOption("30");

  const bar = page.getByTestId("sticky-actions");
  await expect(bar).toBeVisible();
  await expect(bar).toContainText("1 change");
  await expect(page.getByTestId("pending-count")).toHaveText("1");

  // And it is genuinely pinned to the viewport, not just appended to the page.
  // Measured after the slide-in has finished: catching it mid-animation
  // measures the transform, not the layout.
  await page.waitForTimeout(400);
  const box = await bar.boundingBox();
  const viewportHeight = page.viewportSize()?.height ?? 900;
  expect((box?.y ?? 0) + (box?.height ?? 0)).toBeLessThanOrEqual(viewportHeight + 2);

  await shot(page, "device-sticky-apply", info.project.name);

  await page.getByTestId("apply-config").click();
  await expect(page.getByTestId("apply-notice")).toContainText("confirmed 1 change");
  await expect(page.getByTestId("sticky-actions")).toHaveCount(0);
});

test("a typed confirmation rides in the bar with the button it gates", async ({
  page,
}) => {
  await page.goto("/device");

  await page.getByTestId("control-network.lan_service").uncheck();
  const bar = page.getByTestId("sticky-actions");
  await expect(bar).toBeVisible();
  await expect(bar.getByTestId("batch-confirm")).toBeVisible();
  await expect(page.getByTestId("apply-config")).toBeDisabled();

  await page.getByTestId("batch-confirm").fill("LAN OFF");
  await expect(page.getByTestId("apply-config")).toBeEnabled();

  await page.getByTestId("discard-config").click();
  await expect(page.getByTestId("sticky-actions")).toHaveCount(0);
  await expect(page.getByTestId("pending-count")).toHaveText("0");
});

/**
 * Where the consequences live.
 *
 * The fence holds the things that reach the hardware and cannot be undone from
 * this screen: a restart, and a deep sleep only the physical button comes back
 * from. Both are gated by a typed word.
 *
 * "Always ready" is not in the fence any more, and that is the substantive
 * change rather than a cosmetic one. It is a choice about energy — the same
 * kind of choice as Balanced and Deep saver — and standing it in a red box
 * apart from its two siblings made the trade impossible to read. It is now the
 * first of three cards that each state their own cost, and the warning that
 * used to be its only context is printed on the card itself and repeated in the
 * confirmation dialog. Nothing about the gate moved.
 */
test("the consequential actions stay in one fenced place", async ({ page }) => {
  await page.goto("/device");

  const zone = page.getByTestId("power-danger-zone");
  await expect(zone).toBeVisible();
  await expect(zone.getByTestId("action-system.restart")).toBeVisible();
  await expect(zone.getByTestId("action-system.sleep")).toBeVisible();

  // The three energy choices are a choice, not a hazard, so none of them is in
  // the fence — including the expensive one.
  for (const testId of ["power-set-always-on", "power-set-auto", "power-set-saver"]) {
    await expect(zone.getByTestId(testId), `${testId} is in the fence`).toHaveCount(0);
    await expect(page.getByTestId(testId), `${testId} is missing`).toBeVisible();
  }

  // And the expensive one still says what it costs, in place, before it is
  // pressed rather than only in the dialog afterwards.
  await expect(page.getByTestId("power-set-always-on")).toContainText(
    "battery life measured in hours",
  );
});
