import { expect, test } from "@playwright/test";
import { createDashboard, shot, signIn } from "./helpers";

/**
 * The phone, measured rather than eyeballed.
 *
 * Three properties, and all three were false before the refit: the page does
 * not scroll sideways at 375 px, every control a thumb has to hit is at least
 * 44 px in its smaller dimension, and the tab bar is tall enough and its
 * labels large enough to read and to press.
 *
 * The measurements run on whatever viewport the project gives them, so the
 * desktop project checks the same things and simply has an easier time.
 */

const PAGES = ["/overview", "/dashboards", "/device", "/voice", "/diagnostics"];

test.beforeEach(async ({ page }) => {
  await signIn(page);
});

test("no page scrolls sideways", async ({ page }) => {
  const width = page.viewportSize()?.width ?? 1440;

  for (const path of PAGES) {
    await page.goto(path);
    await page.waitForTimeout(600);
    const scrollWidth = await page.evaluate(
      () => document.documentElement.scrollWidth,
    );
    // One pixel of tolerance for sub-pixel layout rounding; anything more is
    // content that does not fit, which is the defect this pins.
    expect(scrollWidth, `${path} overflows horizontally`).toBeLessThanOrEqual(
      width + 1,
    );
  }
});

/**
 * What "Interactive 15 min" does is readable with a finger.
 *
 * The caveat — applied now if the device is awake, held if it is asleep, and
 * never a wake — used to live in a `title` attribute on the button. A `title`
 * is shown to a hovering mouse and to nothing else: not to a touch screen, not
 * to a keyboard. So on the one form factor where this button is most pressed,
 * the sentence that stops it being read as "wake the device up" was not
 * readable at all.
 *
 * This asserts the vehicle, not just the words: the note is behind a control
 * that opens, and it is closed until someone opens it.
 */
test("the interactive caveat can be opened rather than hovered", async ({ page }) => {
  await page.goto("/overview");
  await expect(page.getByTestId("state-strip-interactive")).toBeVisible();

  const why = page.getByTestId("state-strip-interactive-why");
  await expect(why).toBeVisible();
  await expect(why).toHaveAttribute("aria-expanded", "false");

  await why.click();
  await expect(why).toHaveAttribute("aria-expanded", "true");
  await expect(page.getByTestId("state-strip-interactive-why-body")).toContainText(
    "no command from here can wake it",
  );
});

test("the designer does not scroll sideways either", async ({ page }, info) => {
  /**
   * Its own composition, waited for rather than probed.
   *
   * This used to open whichever card an earlier spec happened to have left in
   * the gallery, and counted them the instant `page.goto` resolved. Both
   * halves were unsound. The gallery is a client component that fetches its
   * list in an effect, so a count taken the moment the load event fires is a
   * count of a page still showing its loading state — which is exactly what a
   * production build produces, because there the load event arrives before the
   * list request comes back. And the branch that was supposed to catch an
   * empty gallery filled a title field and clicked a Create button that no
   * longer exist: the template picker replaced them, so the fallback could only
   * ever time out.
   */
  const { id } = await createDashboard(page, `Fit ${info.project.name}`);
  await page.goto("/dashboards");
  const card = page.getByTestId(`dash-${id}`);
  await expect(card).toBeVisible();
  await card.getByRole("link", { name: "Edit" }).click();
  await expect(page.getByTestId("designer-stage")).toBeVisible();
  await page.waitForTimeout(800);

  const width = page.viewportSize()?.width ?? 1440;
  expect(
    await page.evaluate(() => document.documentElement.scrollWidth),
  ).toBeLessThanOrEqual(width + 1);

  // And the canvas is above the fold: it is the reason the page exists.
  const stage = await page.getByTestId("designer-stage").boundingBox();
  const viewportHeight = page.viewportSize()?.height ?? 900;
  expect(stage?.y ?? 9999).toBeLessThan(viewportHeight);
});

test("every control a thumb has to hit is at least 44 px", async ({ page }, info) => {
  test.skip(
    info.project.name === "desktop-chromium",
    "The target size is a touch requirement; the desktop project has a pointer.",
  );

  for (const path of PAGES) {
    await page.goto(path);
    await page.waitForTimeout(600);

    const small = await page.evaluate(() => {
      const nodes = [
        ...document.querySelectorAll<HTMLElement>(
          "button, a.btn, .tabs a, input[type=checkbox], input[type=radio], select",
        ),
      ];

      /**
       * What a finger can actually hit.
       *
       * A 24 px checkbox inside a label is a label-sized target: tapping the
       * words toggles it, and every browser has done that for twenty years. So
       * the box measured for an input is its enclosing <label> when it has
       * one, and the control itself otherwise. Measuring the input alone would
       * report a defect that does not exist, and the fix for it — a 44 px
       * checkbox — would be worse design than the thing it replaced.
       */
      const target = (node: HTMLElement): DOMRect => {
        const label = node.closest("label");
        return (label ?? node).getBoundingClientRect();
      };

      return nodes
        .filter((node) => {
          const rect = node.getBoundingClientRect();
          // Hidden things are not targets.
          if (rect.width === 0 || rect.height === 0) return false;
          const hit = target(node);
          return Math.min(hit.width, hit.height) < 44;
        })
        .map((node) => ({
          text: (node.textContent ?? "").trim().slice(0, 40),
          testId: node.getAttribute("data-testid"),
          height: Math.round(target(node).height),
          width: Math.round(target(node).width),
        }));
    });

    expect(small, `${path} has controls under 44 px`).toEqual([]);
  }
});

test("the tab bar is tall enough and its labels large enough", async ({ page }, info) => {
  test.skip(
    (page.viewportSize()?.width ?? 1440) > 760,
    "This viewport gets the nav row under the header, not the tab bar.",
  );

  await page.goto("/overview");
  const tabs = page.locator("nav.tabs a");
  const count = await tabs.count();
  // Four, not five. Voice left the navigation — it is an experiment that has
  // never been validated on hardware and configures nothing by default, so a
  // permanent quarter of a phone's navigation is more than it has earned. It
  // is reached from the card that describes it, on Device.
  expect(count).toBe(4);

  for (let i = 0; i < count; i += 1) {
    const box = await tabs.nth(i).boundingBox();
    expect(box?.height ?? 0).toBeGreaterThanOrEqual(56);
    const size = await tabs.nth(i).evaluate((node) =>
      Number.parseFloat(getComputedStyle(node).fontSize),
    );
    expect(size).toBeGreaterThanOrEqual(12);
  }

  await shot(page, "mobile-tabs", info.project.name);
});

test("skip to content is the first thing a keyboard reaches", async ({ page }) => {
  await page.goto("/overview");
  await page.keyboard.press("Tab");
  const focused = await page.evaluate(() => document.activeElement?.className ?? "");
  expect(focused).toContain("skip-link");

  await page.keyboard.press("Enter");
  await expect(page.locator("#main")).toBeVisible();
});

/**
 * Affordance parity.
 *
 * The rail this shell replaced was `display: none` below 760 px, and the rail
 * was where Sign out, the density toggle and the device state lived. So on a
 * phone there was no way to sign out of the tower at all — the only route was
 * to widen the window — and the device's own state, which is the fact every
 * other decision on every page depends on, could only be read by navigating to
 * the Device page.
 *
 * This runs on every project in the config, so it is a statement about all
 * three viewports rather than about the one that happened to be broken. It
 * fails at 375 px and at 768 px against the code as it was.
 */
test("every shell affordance is reachable at every viewport", async ({ page }) => {
  // Compositions, because it renders no device card of its own: the shell is
  // the only place the device's state exists on this page, at every width.
  await page.goto("/dashboards");
  // The chip needs the shell's first device read to land.
  await expect(page.getByTestId("device-chip")).toBeVisible();

  for (const testId of ["sign-out", "density-toggle", "device-chip"]) {
    const control = page.getByTestId(testId);
    await expect(control, `${testId} is missing at this width`).toHaveCount(1);
    await expect(control, `${testId} is not visible at this width`).toBeVisible();
  }

  // And exactly one of each in the DOM, so nothing was made reachable by
  // duplicating it into a second layout that can drift from the first. Sign
  // out and the density toggle are at the foot of the page at every width
  // precisely so that there is no second copy to keep in step; the rail this
  // replaced kept them at the top and then hid them below 760 px, which is how
  // the only way to sign out of the tower on a phone became "widen the
  // window".
  expect(
    await page.evaluate(() =>
      ["sign-out", "density-toggle", "device-chip"].map(
        (id) => document.querySelectorAll(`[data-testid="${id}"]`).length,
      ),
    ),
  ).toEqual([1, 1, 1]);
});

/**
 * The device state: everywhere, and never twice in one place.
 *
 * The chip in the header carries the state word on every page, at every width.
 * Overview and Device also carry it in full — the reason, what can be done, and
 * the interactive request — and that pair is a summary and its expansion rather
 * than a duplicate: one is four words in the chrome, the other is three
 * paragraphs in the page.
 *
 * What must never happen is the thing the rail used to do at 375 px, where it
 * reflowed into a strip lying directly on the page and produced two identical
 * badges and two identical "Interactive 15 min" buttons a centimetre apart. So
 * the assertion is: exactly one chip everywhere, and exactly one interactive
 * request on the two pages that offer it and none on the pages that do not.
 */
test("the state is shown everywhere, and the interactive request only once", async ({
  page,
}) => {
  for (const path of ["/overview", "/device"]) {
    await page.goto(path);
    await expect(page.getByTestId("state-strip")).toBeVisible();
    await expect(page.getByTestId("device-chip")).toHaveCount(1);
    await expect(
      page.locator('[data-testid="state-strip-interactive"]').filter({ visible: true }),
      `${path} shows the wrong number of interactive requests`,
    ).toHaveCount(1);
  }

  // The pages without one keep the chip, which on those pages is the only
  // reading of the device there is. Hiding it by route or by breakpoint would
  // take the device state away from three of the five pages.
  for (const path of ["/dashboards", "/voice", "/diagnostics"]) {
    await page.goto(path);
    await expect(page.getByTestId("device-chip")).toBeVisible();
    await expect(page.getByTestId("state-strip")).toHaveCount(0);
    await expect(
      page.locator('[data-testid="state-strip-interactive"]'),
      `${path} grew an interactive request it has no context for`,
    ).toHaveCount(0);
  }
});

test("the density toggle actually works from a phone", async ({ page }) => {
  await page.goto("/overview");
  const toggle = page.getByTestId("density-toggle");
  await expect(toggle).toBeVisible();

  const before = await page.locator(".shell").getAttribute("data-density");
  await toggle.click();
  await expect(page.locator(".shell")).not.toHaveAttribute(
    "data-density",
    before ?? "comfortable",
  );

  // Put it back, so the rest of the suite sees the spacing it expects.
  await toggle.click();
  await expect(page.locator(".shell")).toHaveAttribute(
    "data-density",
    before ?? "comfortable",
  );
});

/**
 * The header does not slide under the banner it is offset from.
 *
 * The header is `position: sticky; top: var(--banner-height)`, and that offset
 * is published by a ResizeObserver rather than guessed, because the banner's
 * sentence wraps between 760 px and about 1100 px. Get it wrong and the header
 * sticks to the top of the viewport and scrolled content shows through the gap
 * — or worse, the banner's bottom border cuts across the nav row.
 *
 * The ancestor of this test measured the foot of a full-height rail for the
 * same reason: the rail started below the banner and was a full viewport tall,
 * so its last few pixels — which held Sign out — hung off the bottom of the
 * window. Those controls are at the foot of the page now, so what is left to
 * pin is the offset itself.
 */
test("the sticky header stays clear of the banner it is offset from", async ({ page }, info) => {
  test.skip(
    (page.viewportSize()?.width ?? 1440) <= 760,
    "Neither the banner nor the header is sticky below 760 px.",
  );

  await page.goto("/overview");
  await expect(page.getByTestId("simulated-banner")).toBeVisible();

  // Scrolled well down, which is the only state where the offset can be wrong:
  // at the top everything is in flow and any offset looks right.
  await page.evaluate(() => window.scrollTo(0, 600));
  await page.waitForTimeout(200);

  const banner = await page.getByTestId("simulated-banner").boundingBox();
  const header = await page.locator("header.app-header").boundingBox();
  expect(banner, "the banner has no box").not.toBeNull();
  expect(header, "the header has no box").not.toBeNull();

  // The banner is pinned at the top and the header sits immediately under it,
  // with no scrolled content showing between the two and no overlap.
  expect(banner?.y ?? -1, "the banner is not pinned to the top").toBeLessThanOrEqual(1);
  const gap = (header?.y ?? 0) - ((banner?.y ?? 0) + (banner?.height ?? 0));
  expect(Math.abs(gap), "the header and the banner have come apart").toBeLessThanOrEqual(2);

  await shot(page, "header-under-banner", info.project.name);
});
