import { expect, test, type Page } from "@playwright/test";
import { setMock, signIn } from "./helpers";

/**
 * How long a page waits for a panel before it will draw anything.
 *
 * WHAT THIS SUITE COULD NOT REACH BEFORE
 * --------------------------------------
 * Every spec in this directory ran against a mock that either answered at once
 * or failed at once (`asleep`, which destroys the socket in a millisecond).
 * The production failure is neither: a socket that is *accepted* and never
 * answered, so the caller pays its own ten-second timeout. That is what a
 * dropped packet, a firewall, or a `node` binary without macOS Local Network
 * permission looks like from this side, and it is why a regression that made
 * `/overview` and `/device` unusable for twenty and thirty seconds could not be
 * seen in CI at all. `MockDevice.stallMs` is that case, and this file is the
 * first spec to use it.
 *
 * WHAT IS BEING PINNED
 * --------------------
 * Both pages paint from what the tower already knows — the last confirmed
 * reading, the ledger, the selection, the address — and the read that
 * establishes what is true *now* runs behind them. So every assertion here is
 * some form of the same sentence: a device that never answers costs a badge,
 * not a page.
 *
 * The honesty half is pinned alongside it, because the cheap way to make a
 * page fast is to let it claim things. Nothing here may render UNREACHABLE, or
 * a confident "Asleep", on the strength of a read that has not happened.
 */

/**
 * Longer than the device read timeout, so the stall is never outlived.
 *
 * Ten seconds is `DEVICE_TIMEOUT_MS`, and the manual budget on top of it is
 * twenty-one. A read that starts inside this window does not come back inside
 * it, which is the condition every assertion below is made under.
 */
const NEVER_ANSWERS_MS = 30_000;

/** What "immediately" means here. Generous, and still two orders off the bug. */
const FIRST_PAINT_BUDGET_MS = 1_000;

/**
 * Compile both pages before anything is timed.
 *
 * The browser suite runs `next dev`, which builds a route the first time it is
 * asked for. That is seconds, it has nothing to do with the device, and timing
 * it would make this file measure the bundler. Every measured navigation below
 * is therefore a second visit.
 */
async function warm(page: Page): Promise<void> {
  await page.goto("/overview");
  await expect(page.getByTestId("state-strip")).toBeVisible();
  await page.goto("/device");
  await expect(page.getByTestId("state-strip")).toBeVisible();
}

/** How long until `locator` is on screen, from the moment navigation starts. */
async function timeToPaint(
  page: Page,
  path: string,
  testId: string,
): Promise<number> {
  const started = Date.now();
  await page.goto(path);
  await expect(page.getByTestId(testId)).toBeVisible();
  return Date.now() - started;
}

test.describe("a device that never answers costs a badge, not a page", () => {
  test.beforeEach(async ({ page }) => {
    await signIn(page);
    await setMock(page, { reset: true });
    await warm(page);
  });

  test.afterEach(async ({ page }) => {
    // Put the panel back on the network for whatever runs next. The stall is a
    // transport condition and `reset` clears it; this is belt and braces for a
    // spec that failed part way through.
    await setMock(page, { stallMs: 0, reset: true }).catch(() => undefined);
  });

  test("/overview draws its structure while the device read hangs", async ({
    page,
  }) => {
    await setMock(page, { stallMs: NEVER_ANSWERS_MS });

    const ms = await timeToPaint(page, "/overview", "state-strip");
    expect(ms).toBeLessThan(FIRST_PAINT_BUDGET_MS);

    // Not a skeleton: the real page, with the parts that do not need a panel.
    await expect(page.getByTestId("page-loading")).toHaveCount(0);
    await expect(page.getByTestId("on-panel")).toBeVisible();
    await expect(page.getByTestId("refresh")).toBeEnabled();

    // And every block that would otherwise be asserting something says, in
    // its own words, that it has not asked yet. Each of these replaced a
    // sentence that was a claim: "no answer · last tried" on a read never
    // made, "Select a composition" to somebody who had selected one, and
    // "Nothing is stored on the device yet" about a panel nobody had asked.
    await expect(page.getByTestId("panel-not-read")).toBeVisible();
    await expect(page.getByTestId("sources-not-read")).toBeVisible();
    await expect(page.getByTestId("panel-unread")).toBeVisible();
    await expect(page.getByTestId("panel-label")).toHaveText("ON THE PANEL");
  });

  test("/device draws the last known state and its controls", async ({ page }) => {
    await setMock(page, { stallMs: NEVER_ANSWERS_MS });

    const ms = await timeToPaint(page, "/device", "state-strip");
    expect(ms).toBeLessThan(FIRST_PAINT_BUDGET_MS);

    await expect(page.getByTestId("page-loading")).toHaveCount(0);
    // The controls that do not depend on the network are usable at once. A
    // mode change made now is recorded and delivered at the next wake, which
    // is the whole point of the intent queue.
    await expect(page.getByTestId("power-no-remote-wake")).toBeVisible();
    await expect(page.getByTestId("power-set-saver")).toBeEnabled();
    await expect(page.getByTestId("state-strip-interactive")).toBeEnabled();
    await expect(page.getByTestId("refresh")).toBeEnabled();
  });

  test("claims nothing about a read that has not happened", async ({ page }) => {
    await setMock(page, { stallMs: NEVER_ANSWERS_MS });
    await page.goto("/device");
    await expect(page.getByTestId("state-strip")).toBeVisible();

    // The exact defect PR #6 corrected, arriving from the other direction: a
    // red UNREACHABLE banner underneath a badge, derived from a read that in
    // this case was not merely failed but never made.
    await expect(page.getByTestId("device-unreachable")).toHaveCount(0);
    await expect(page.getByTestId("state-strip")).not.toHaveAttribute(
      "data-device-state",
      "unreachable",
    );

    // Nor the configuration banner, which names an api number the fallback
    // invented — `negotiate({api: 1})` — rather than one the device reported.
    await expect(page.getByTestId("config-not-read")).toBeVisible();
    await expect(page.getByTestId("config-unsupported")).toHaveCount(0);
  });

  test("navigation between Overview and Device stays instant during a read", async ({
    page,
  }) => {
    await setMock(page, { stallMs: NEVER_ANSWERS_MS });
    await page.goto("/overview");
    await expect(page.getByTestId("state-strip")).toBeVisible();

    // Four crossings while reads are piling up behind the device mutex. Each
    // one used to cost the reader a fresh pair of timeouts.
    for (let pass = 0; pass < 2; pass += 1) {
      const toDevice = Date.now();
      await page.getByRole("navigation", { name: "Main" }).first()
        .getByRole("link", { name: "Device" }).click();
      await expect(page.getByTestId("power-no-remote-wake")).toBeVisible();
      expect(Date.now() - toDevice).toBeLessThan(FIRST_PAINT_BUDGET_MS * 3);

      const toOverview = Date.now();
      await page.getByRole("navigation", { name: "Main" }).first()
        .getByRole("link", { name: "Overview" }).click();
      await expect(page.getByTestId("on-panel")).toBeVisible();
      expect(Date.now() - toOverview).toBeLessThan(FIRST_PAINT_BUDGET_MS * 3);
    }
  });

  test("Check now immobilises its own button and nothing else", async ({ page }) => {
    await setMock(page, { stallMs: NEVER_ANSWERS_MS });
    await page.goto("/device");
    await expect(page.getByTestId("state-strip")).toBeVisible();

    const check = page.getByTestId("refresh");
    await check.click();

    // Its own indicator says what it is doing, rather than looking broken.
    await expect(check).toHaveText("Checking…");
    await expect(check).toBeDisabled();

    // Meanwhile the rest of the page is alive: the controls that do not depend
    // on this read are still operable, and navigation still works.
    await expect(page.getByTestId("power-set-saver")).toBeEnabled();
    await expect(page.getByTestId("state-strip-interactive")).toBeEnabled();

    const away = Date.now();
    await page.getByRole("navigation", { name: "Main" }).first()
      .getByRole("link", { name: "Overview" }).click();
    await expect(page.getByTestId("on-panel")).toBeVisible();
    expect(Date.now() - away).toBeLessThan(FIRST_PAINT_BUDGET_MS * 3);
  });

  test("a background scheduler pass does not block the pages", async ({ page }) => {
    // `runDeviceTick` is the real function the LaunchAgent runs on its timer,
    // executed once, synchronously, against a device that will not answer. It
    // takes the device mutex for the whole of its read.
    await setMock(page, { stallMs: NEVER_ANSWERS_MS });
    const tick = setMock(page, { runDeviceTick: true }).catch(() => undefined);

    const ms = await timeToPaint(page, "/overview", "state-strip");
    expect(ms).toBeLessThan(FIRST_PAINT_BUDGET_MS);
    await expect(page.getByTestId("page-loading")).toHaveCount(0);

    await setMock(page, { stallMs: 0 });
    await tick;
  });
});

test.describe("the reading replaces the record when it lands", () => {
  test.beforeEach(async ({ page }) => {
    await signIn(page);
    await setMock(page, { reset: true });
    await warm(page);
  });

  test("a successful read replaces the first paint with Awake", async ({ page }) => {
    // The mock answers `awake: true`. The first paint cannot know that — it
    // has not asked — so it must start at something honest and then be
    // replaced, rather than being right by luck.
    await page.goto("/device");
    const strip = page.getByTestId("state-strip");
    await expect(strip).toBeVisible();
    await expect(strip).toHaveAttribute("data-device-state", "awake");
    await expect(page.getByTestId("state-strip-badge")).toHaveText("Awake");
  });

  test("Overview settles from not-read to a real reading on its own", async ({
    page,
  }) => {
    await page.goto("/overview");
    await expect(page.getByTestId("state-strip")).toBeVisible();

    // No click, no reload: the page asked again by itself because the payload
    // it got said the server was still reading. Well inside the sixty-second
    // poll it would otherwise have waited for.
    await expect(page.getByTestId("panel-not-read")).toHaveCount(0, {
      timeout: 15_000,
    });
    await expect(page.getByTestId("panel-provenance")).toContainText(
      "device-reported",
    );
  });

  test("a sleeping device is an ordinary state, drawn at once", async ({ page }) => {
    // First a real read, so the tower has something the device itself said.
    await page.goto("/device");
    await expect(page.getByTestId("state-strip")).toHaveAttribute(
      "data-device-state",
      "awake",
    );

    // Then the panel goes away the way a sleeping one does: nothing listening.
    await setMock(page, { asleep: true });
    const ms = await timeToPaint(page, "/device", "state-strip");
    expect(ms).toBeLessThan(FIRST_PAINT_BUDGET_MS);
    await expect(page.getByTestId("page-loading")).toHaveCount(0);

    await setMock(page, { reset: true, asleep: false }).catch(() => undefined);
  });
});
