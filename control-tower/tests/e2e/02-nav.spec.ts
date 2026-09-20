import { expect, test } from "@playwright/test";
import { shot, signIn } from "./helpers";

test.beforeEach(async ({ page }) => {
  await signIn(page);
});

/**
 * The four navigation entries, and the pages behind them.
 *
 * `label` is what the navigation says; `heading` is what the page says. They
 * differ in one place on purpose: the entry reads "Advanced", because from the
 * outside that is what it is — somewhere you go when something is wrong — while
 * the page itself is titled Diagnostics.
 *
 * Voice is not one of the four and is reached from the card that describes it,
 * on Device. It is still a route, and it is still asserted below: a page
 * dropping out of the navigation must not mean it drops out of the QA.
 */
const NAV_PAGES = [
  { path: "/overview", label: "Overview", heading: "Overview" },
  { path: "/dashboards", label: "Compositions", heading: "Compositions" },
  { path: "/device", label: "Device", heading: "Device" },
  { path: "/diagnostics", label: "Advanced", heading: "Diagnostics" },
] as const;

/** Every page with a shell around it, navigable or not. */
const PAGES = [
  ...NAV_PAGES,
  { path: "/voice", label: "Voice", heading: "Voice" },
] as const;

/**
 * Which nav a viewport gets, decided the way the stylesheet decides it.
 *
 * Keyed on the width rather than on the project name, so adding a form factor
 * to playwright.config.ts does not silently assert the wrong layout: the
 * horizontal nav under the header appears above 760 px, which is the one
 * breakpoint in app.css.
 */
function usesTabs(page: { viewportSize: () => { width: number } | null }): boolean {
  return (page.viewportSize()?.width ?? 1440) <= 760;
}

test("every page reaches every other page from the nav", async ({ page }) => {
  await page.goto("/overview");
  // A row under the sticky header on desktop, a bottom tab bar on mobile.
  // Both are labelled "Main" and exactly one of them exists at a given width.
  const nav = page.locator(usesTabs(page) ? "nav.tabs" : "nav.top-nav");
  await expect(nav).toBeVisible();

  for (const target of NAV_PAGES) {
    await nav.getByRole("link", { name: target.label, exact: true }).click();
    await page.waitForURL(`**${target.path}`);
    // Overview's h1 is visually hidden — the prototype gives that screen no
    // page title, because the nav already says where you are and the first
    // thing on it should be the panel. It is still a heading, still first in
    // the document, and still the thing a screen reader announces, which is
    // why this asserts the heading exists rather than that it is painted.
    await expect(
      page.getByRole("heading", { name: target.heading, level: 1 }),
    ).toHaveCount(1);
    await expect(
      nav.getByRole("link", { name: target.label, exact: true }),
    ).toHaveAttribute("aria-current", "page");
  }
});

test("the correct nav is shown for the form factor", async ({ page }) => {
  await page.goto("/overview");
  // The header and the device chip are there at every width: what the device
  // is doing is the fact every other decision depends on, and it is the one
  // thing worth a permanent row on a phone.
  await expect(page.locator("header.app-header")).toBeVisible();
  await expect(page.getByTestId("device-chip")).toBeVisible();

  if (usesTabs(page)) {
    await expect(page.locator("nav.tabs")).toBeVisible();
    await expect(page.locator("nav.top-nav")).toBeHidden();
  } else {
    await expect(page.locator("nav.top-nav")).toBeVisible();
    await expect(page.locator("nav.tabs")).toBeHidden();
  }
});

/**
 * Exactly one navigation landmark called "Main", at every width.
 *
 * There are two `<nav aria-label="Main">` elements in the markup — the row
 * under the header and the bottom tab bar — and the layout hides one of them
 * with `display: none` at every width. That is what makes this pass, and it is
 * not an accident of CSS: an element that is merely visually hidden, or moved
 * off-screen, would still be a landmark, and somebody listing landmarks would
 * be offered two identically named regions with one of them unreachable. The
 * ancestor of this assertion was a rail that kept Sign out on a phone while
 * hiding its link list, and so announced an empty navigation above a full one.
 */
test("there is one Main navigation landmark, whatever the width", async ({ page }) => {
  for (const target of PAGES) {
    await page.goto(target.path);
    await expect(
      page.getByRole("heading", { name: target.heading, level: 1 }),
    ).toHaveCount(1);
    // getByRole ignores anything `display: none` puts out of the tree, which is
    // precisely the distinction being tested.
    await expect(page.getByRole("navigation", { name: "Main" })).toHaveCount(1);
  }
});

test("the simulated banner is on every page while the device is mocked", async ({
  page,
}) => {
  for (const target of PAGES) {
    await page.goto(target.path);
    await expect(page.getByTestId("simulated-banner")).toBeVisible();
    await expect(page.getByTestId("simulated-banner")).toContainText("SIMULATED");
  }
});

test("capture every page", async ({ page }, info) => {
  for (const target of PAGES) {
    await page.goto(target.path);
    await expect(
      page.getByRole("heading", { name: target.heading, level: 1 }),
    ).toHaveCount(1);
    // Let the first data read land so the screenshot is not of a loading state.
    await page.waitForTimeout(1200);
    await shot(page, target.path.replace("/", ""), info.project.name);
  }
});
