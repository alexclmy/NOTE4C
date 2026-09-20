import { expect, test } from "@playwright/test";
import { setMock, shot, signIn } from "./helpers";

/**
 * One word for what the device is doing, on every page that says it.
 *
 * Before `deriveDeviceState` there were three opinions: Overview drew a red
 * UNREACHABLE, the power panel drew a yellow "Not answering", and the shell
 * drew nothing at all — for a device that was asleep exactly as designed. The
 * word is now computed once, and these specs check that the same reading
 * produces the same word wherever it is rendered.
 */

test.beforeEach(async ({ page }) => {
  await signIn(page);
  await setMock(page, { reset: true, panelDelayMs: 50, asleep: false });
});

test.afterEach(async ({ page }) => {
  // Never leave the mock silent for the next spec file.
  await setMock(page, { asleep: false }).catch(() => undefined);
});

test("an answering device reads AWAKE on Overview and on Device", async ({ page }) => {
  await page.goto("/overview");
  await expect(page.getByTestId("state-strip")).toHaveAttribute(
    "data-state",
    "awake",
  );

  await page.goto("/device");
  await expect(page.getByTestId("state-strip")).toHaveAttribute(
    "data-state",
    "awake",
  );
  await expect(page.getByTestId("state-strip-badge")).toHaveText("Awake");
});

test("a silent mock is UNREACHABLE, because a mock has no sleep to be in", async ({
  page,
}, info) => {
  await setMock(page, { asleep: true });

  await page.goto("/overview");
  const strip = page.getByTestId("state-strip");
  await expect(strip).toHaveAttribute("data-state", "unreachable");
  // And it explains itself, rather than showing a colour with no account.
  await expect(strip).toContainText("mock");
  await shot(page, "overview-unreachable", info.project.name);

  await page.goto("/device");
  await expect(page.getByTestId("state-strip")).toHaveAttribute(
    "data-state",
    "unreachable",
  );
  await expect(page.getByTestId("state-strip-badge")).toHaveText("Unreachable");
});

test("a device that has not taken up its requested mode reads PENDING", async ({
  page,
}) => {
  await setMock(page, { powerPendingWake: true });

  await page.goto("/device");
  await expect(page.getByTestId("state-strip-badge")).toHaveText("Pending");
  await expect(page.getByTestId("state-strip")).toHaveAttribute(
    "data-state",
    "pending",
  );

  await setMock(page, { powerPendingWake: false });
});

test("the state strip carries the one-click interactive request", async ({ page }) => {
  await page.goto("/overview");
  const button = page.getByTestId("state-strip-interactive");
  await expect(button).toBeVisible();

  await button.click();
  // Applied or held — never "done". Both wordings are honest; a click that
  // produced neither would be the bug. It is reported in a toast rather than a
  // banner that stays until the next navigation, because the outcome is small,
  // reversible and about something that has already finished. Anything that
  // touched the panel gets a dialog instead.
  await expect(page.getByTestId("toast")).toContainText(
    /applied to the device|held until the device next wakes/,
  );
});

test("Overview says when its reading has gone stale", async ({ page }) => {
  // The clock is driven forward rather than waited out: the banner appears at
  // twice the poll interval, which is two real minutes of nothing happening.
  await page.clock.install();
  await page.goto("/overview");
  await expect(page.getByTestId("state-strip")).toBeVisible();

  // Far enough past 2x60 s that the staleness test cannot be a near miss, and
  // with the tab hidden so the poll itself does not refresh it away.
  await page.evaluate(() => {
    Object.defineProperty(document, "visibilityState", {
      value: "hidden",
      configurable: true,
    });
    document.dispatchEvent(new Event("visibilitychange"));
  });
  await page.clock.runFor(200_000);

  const banner = page.getByTestId("stale-banner");
  await expect(banner).toBeVisible();
  // The banner is about the TOWER not answering, which is a different fact
  // from the server's device snapshot being older than its TTL. The second is
  // reported next to the timestamp in the page head, because the tower is
  // answering perfectly well and simply has not re-read the panel yet.
  await expect(banner).toContainText("has not answered for more than");

  await page.getByTestId("stale-retry").click();
  await expect(page.getByTestId("stale-banner")).toBeHidden();
});
