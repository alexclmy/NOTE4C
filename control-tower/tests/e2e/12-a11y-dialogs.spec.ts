import { expect, test } from "@playwright/test";
import { signIn } from "./helpers";

/**
 * The dialogs, from a keyboard.
 *
 * These guard PUSH, RESTART, SLEEP and REAL DEVICE — every operation in this
 * product that reaches hardware. The previous implementation set
 * `aria-modal="true"` and stopped there: Tab walked straight out into the page
 * behind, Escape did nothing, focus was never returned to the control that
 * opened it, and the page underneath still scrolled. "Modal" was a claim in an
 * attribute rather than a property of the thing.
 */

test.beforeEach(async ({ page }) => {
  await signIn(page);
});

test("focus enters the dialog, cycles inside it, and comes back on close", async ({
  page,
}) => {
  await page.goto("/device");

  const trigger = page.getByTestId("action-system.restart");
  await trigger.click();

  const dialog = page.getByRole("dialog");
  await expect(dialog).toBeVisible();

  // The typed confirmation is where the work is, so that is where focus lands.
  await expect(page.getByTestId("confirm-word")).toBeFocused();

  // And it lands there because it is *marked*, not because it happens to be
  // the first focusable element in the panel. The dialog used to take
  // `querySelector(FOCUSABLE)`, so the rule held only for as long as no dialog
  // body contained a link or a button; the first one that did would have moved
  // focus off the confirmation field with nothing to notice it.
  await expect(page.getByTestId("confirm-word")).toHaveAttribute(
    "data-dialog-autofocus",
    "",
  );

  // Tab all the way round: whatever the count of focusables, the focus never
  // leaves the dialog. Ten presses is comfortably more than it holds.
  for (let i = 0; i < 10; i += 1) {
    await page.keyboard.press("Tab");
    const inside = await page.evaluate(() => {
      const active = document.activeElement;
      const panel = document.querySelector('[role="dialog"]');
      return Boolean(active && panel && panel.contains(active));
    });
    expect(inside, `focus left the dialog after ${i + 1} tabs`).toBe(true);
  }

  // Backwards, too.
  await page.keyboard.press("Shift+Tab");
  expect(
    await page.evaluate(() => {
      const panel = document.querySelector('[role="dialog"]');
      return Boolean(panel && panel.contains(document.activeElement));
    }),
  ).toBe(true);

  await page.keyboard.press("Escape");
  await expect(dialog).toBeHidden();
  await expect(trigger).toBeFocused();
});

test("the page behind a dialog does not scroll", async ({ page }) => {
  await page.goto("/device");
  const before = await page.evaluate(() => document.body.style.overflow);

  await page.getByTestId("action-system.restart").click();
  await expect(page.getByRole("dialog")).toBeVisible();
  expect(await page.evaluate(() => document.body.style.overflow)).toBe("hidden");

  await page.getByTestId("confirm-cancel").click();
  expect(await page.evaluate(() => document.body.style.overflow)).toBe(before);
});

test("only ever one overlay is mounted", async ({ page }) => {
  await page.goto("/device");

  await page.getByTestId("action-system.restart").click();
  await expect(page.getByRole("dialog")).toHaveCount(1);

  await page.keyboard.press("Escape");
  await expect(page.getByRole("dialog")).toHaveCount(0);

  await page.getByTestId("action-system.sleep").click();
  await expect(page.getByRole("dialog")).toHaveCount(1);
  // And the bottom sheet is not a second one hiding behind it.
  await expect(page.getByTestId("module-sheet")).toHaveCount(0);
  await page.getByTestId("confirm-cancel").click();
});

test("a destructive dialog stays shut until the exact word is typed", async ({
  page,
}) => {
  await page.goto("/device");
  await page.getByTestId("action-system.sleep").click();

  await expect(page.getByTestId("confirm-ok")).toBeDisabled();
  await page.getByTestId("confirm-word").fill("sleep");
  await expect(page.getByTestId("confirm-ok")).toBeDisabled();
  await page.getByTestId("confirm-word").fill("SLEEP");
  await expect(page.getByTestId("confirm-ok")).toBeEnabled();

  // Escape is always a cancel, even here: cancelling is the safe direction,
  // and what protects the panel is the word, not the difficulty of closing.
  await page.keyboard.press("Escape");
  await expect(page.getByRole("dialog")).toBeHidden();
});

test("with reduced motion asked for, nothing animates", async ({ page }) => {
  await page.emulateMedia({ reducedMotion: "reduce" });
  await page.goto("/device");

  const durations = await page.evaluate(() => {
    const style = getComputedStyle(document.documentElement);
    return [
      style.getPropertyValue("--motion-fast").trim(),
      style.getPropertyValue("--motion-slide").trim(),
    ];
  });
  expect(durations).toEqual(["0ms", "0ms"]);
});

test("with no typed confirmation, focus goes to the dialog and not to a button", async ({
  page,
}) => {
  // The send dialog against the mock carries no confirmation word — typing
  // PUSH to reach a simulator would be ceremony — so it is the case the rule's
  // second half is about. The old implementation focused the first focusable
  // element, which here is the close button in the dialog's own header: the
  // screen reader never got handed the dialog, and the keyboard landed on the
  // one control the operator did not open it for.
  await page.goto("/overview");
  await page.getByTestId("push-button").click();

  const dialog = page.getByRole("dialog");
  await expect(dialog).toBeVisible();
  await expect(page.getByTestId("send-review")).toBeVisible();
  await expect(page.getByTestId("send-confirm-word")).toHaveCount(0);

  await expect(dialog).toBeFocused();
  await expect(page.getByTestId("send-flow-close")).not.toBeFocused();
  await expect(page.getByTestId("send-cancel")).not.toBeFocused();

  await page.keyboard.press("Escape");
  await expect(dialog).toBeHidden();
});
