import { expect, test } from "@playwright/test";
import { completeFirstRun, resetDataRoot, shot, signIn } from "./helpers";

/**
 * The first session.
 *
 * The tower already recorded `firstRunCompletedAt` and already accepted a
 * PATCH to set it; nothing in the interface had ever read either. So a new
 * installation opened on a page full of device readings from a mock it had not
 * been told about, with no route to a first dashboard.
 *
 * This spec runs serially and wipes the data root, because "the first session"
 * is a property of a store that has never been used.
 */

test.describe.configure({ mode: "serial" });

test("a fresh tower explains itself, once", async ({ page }, info) => {
  resetDataRoot();
  await completeFirstRun(page);

  const card = page.getByTestId("onboarding");
  await expect(card).toBeVisible();
  await expect(card).toContainText("Nothing here is hardware");
  await expect(card).toContainText("first composition");
  await shot(page, "onboarding", info.project.name);

  // It links onward rather than trapping anybody in a wizard.
  await expect(card.getByRole("link", { name: "Compositions" })).toBeVisible();
  await expect(card.getByRole("link", { name: "Advanced" })).toBeVisible();

  await page.getByTestId("dismiss-onboarding").click();
  await expect(card).toBeHidden();

  // It stays dismissed across a reload, because the tower recorded it.
  await page.reload();
  await expect(page.getByTestId("onboarding")).toHaveCount(0);

  const state = await page.request.get("/api/state");
  const body = (await state.json()) as { firstRunCompletedAt: string | null };
  expect(body.firstRunCompletedAt).not.toBeNull();
});

test("and does not come back on the next sign-in", async ({ page }) => {
  await signIn(page);
  await page.goto("/overview");
  await expect(page.getByTestId("onboarding")).toHaveCount(0);
});

/**
 * How much of a phone screen the chrome is allowed to take before the panel.
 *
 * The first screenful of a first run used to be: a three-line SIMULATED
 * banner, the state strip, and three paragraphs of welcome — so the first
 * thing a new user saw of a product about an e-paper panel was advice about a
 * panel that was somewhere below the fold. The banner now carries a short
 * wording below 760 px and the welcome card is three one-line steps.
 *
 * The numbers below are the measured heights plus headroom, so that the next
 * person who adds a sentence to either finds out at once. They are deliberately
 * not tight: this pins a property — the chrome stays small — not a layout. At
 * the time of writing the banner measures about 52 px and the welcome card
 * about 301 px on a 375x667 screen, against 374 px for the card before the
 * copy was cut.
 */
const PHONE_BANNER_MAX_PX = 64;
const PHONE_ONBOARDING_MAX_PX = 330;

test("on a phone, the chrome above the panel stays small", async ({ page }, info) => {
  test.skip(
    (page.viewportSize()?.width ?? 1440) > 760,
    "This is about a 375 px screen; a desktop has the room.",
  );

  resetDataRoot();
  await completeFirstRun(page);
  await expect(page.getByTestId("onboarding")).toBeVisible();

  const banner = await page.getByTestId("simulated-banner").boundingBox();
  expect(banner?.height ?? 0, "the SIMULATED banner is taller than two lines")
    .toBeLessThanOrEqual(PHONE_BANNER_MAX_PX);
  // And it still says the load-bearing thing, in the short wording.
  await expect(page.getByTestId("simulated-banner")).toContainText("in-repo mock");

  const onboarding = await page.getByTestId("onboarding").boundingBox();
  expect(onboarding?.height ?? 0, "the welcome card has grown").toBeLessThanOrEqual(
    PHONE_ONBOARDING_MAX_PX,
  );

  // The thing the whole product is about, measured from the top of the
  // document rather than the viewport: how far a new user has to scroll on
  // their first visit before they see the panel at all.
  const panelTop = await page
    .getByTestId("on-panel")
    .evaluate((node) => node.getBoundingClientRect().top + window.scrollY);
  const viewportHeight = page.viewportSize()?.height ?? 667;
  expect(
    panelTop,
    "the panel card is more than one screenful down on a first run",
  ).toBeLessThanOrEqual(viewportHeight * 2);

  await shot(page, "onboarding-chrome", info.project.name);
});
