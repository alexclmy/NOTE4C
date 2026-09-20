import { expect, test, type Page } from "@playwright/test";
import {
  createDistinctDashboard,
  csrfFrom,
  selectDashboard,
  setMock,
  shot,
  signIn,
} from "./helpers";

/**
 * The failure path that matters most: the tower sent bytes, the panel never
 * confirmed, and the product has to carry "we do not know" rather than
 * rounding it to success or to failure.
 */

test.beforeEach(async ({ page }) => {
  await signIn(page);
});

test.afterEach(async ({ page }) => {
  // Leave the mock healthy for whatever runs next.
  await page.goto("/overview");
  await setMock(page, { failAck: false, panelDelayMs: 700 });
});

test("an unacknowledged panel makes the push uncertain and blocks the next one", async ({
  page,
}, info) => {
  const { id } = await createDistinctDashboard(
    page,
    `Uncertain ${info.project.name}`,
  );
  await selectDashboard(page, id);

  await page.goto("/overview");
  await setMock(page, { failAck: true, panelDelayMs: 50 });

  await page.getByTestId("push-button").click();
  await page.getByTestId("send-confirm").click();

  // The poll curve runs its full budget before giving up, so this is slow on
  // purpose: refusing to guess early is the behaviour under test. The dialog
  // ends on a screen of its own rather than on "done" or on "failed", because
  // neither of those words is true about a frame the device never confirmed.
  const uncertain = page.getByTestId("send-uncertain");
  await expect(uncertain).toBeVisible({ timeout: 120_000 });
  await expect(uncertain).toContainText("never confirmed");
  await expect(uncertain).toContainText("will not guess");
  await page.getByRole("link", { name: "Go to Overview" }).click();
  await page.waitForURL("**/overview");

  const banner = page.getByTestId("blocking-banner");
  await expect(banner).toBeVisible();
  await expect(banner).toContainText("uncertain");
  await expect(banner).toContainText("Sending is paused until this is resolved");
  await shot(page, "overview-uncertain", info.project.name);

  // And a new send is refused rather than quietly attempted. The button stays
  // pressable on purpose: it reaches a screen that says what is in the way and
  // points at where to resolve it, which a greyed-out control with no account
  // of itself does not.
  await page.getByTestId("push-button").click();
  await expect(page.getByTestId("send-blocked")).toContainText(
    "still unresolved",
  );
  await page.getByTestId("send-flow-close").click();

  // Re-checking while the panel is still wrong must not clear the block.
  await page.getByTestId("recheck").click();
  await expect(page.getByTestId("blocking-banner")).toBeVisible();

  // The panel recovers; re-checking now confirms it from the device itself.
  await setMock(page, { failAck: false, panelDelayMs: 50 });
  await page.evaluate(async () => {
    const token = document.cookie
      .split(";")
      .map((p) => p.trim())
      .find((p) => p.startsWith("note4c_tower_csrf="))
      ?.slice("note4c_tower_csrf=".length);
    await fetch("/api/device/push", {
      method: "POST",
      headers: {
        "content-type": "application/json",
        "x-csrf-token": decodeURIComponent(token ?? ""),
      },
      body: JSON.stringify({}),
    });
  });

  await page.reload();
  await expect(page.getByTestId("blocking-banner")).toBeVisible();
  await page.getByTestId("acknowledge").click();

  await expect(page.getByTestId("blocking-banner")).toBeHidden();
  await expect(page.getByTestId("push-button")).toBeEnabled();

  // Acknowledging is a human accepting an unknown outcome, and the audit log
  // says exactly that.
  await page.goto("/diagnostics");
  const audit = page.getByTestId("audit-table");
  await expect(audit).toContainText("device.push.acknowledge");
  await page.getByTestId("diag-tab-history").click();
  await expect(page.getByTestId("ledger-table")).toContainText("acknowledged");
});

test("a slow panel shows STORED before DISPLAYED", async ({ page }, info) => {
  const { id } = await createDistinctDashboard(
    page,
    `Slow panel ${info.project.name}`,
  );
  await selectDashboard(page, id);

  await page.goto("/overview");
  // Long enough that stored and displayed are observably different, short
  // enough that the spec stays inside its budget.
  await setMock(page, { failAck: false, panelDelayMs: 3_000 });

  const csrf = await csrfFrom(page);
  const pushInFlight = page.request.post("/api/device/push", {
    headers: { "content-type": "application/json", "x-csrf-token": csrf },
    data: { force: true },
    timeout: 60_000,
  });

  const storedEqualsDisplayed = async (): Promise<boolean | null> => {
    const response = await page.request.get("/api/device/status");
    const body = (await response.json()) as { storedEqualsDisplayed?: boolean };
    return body.storedEqualsDisplayed ?? null;
  };

  // While the panel develops, the tower reports stored and NOT displayed.
  // That gap is the whole reason both are reported.
  await expect
    .poll(storedEqualsDisplayed, { timeout: 10_000, intervals: [150] })
    .toBe(false);

  const result = await pushInFlight;
  expect(result.status(), await result.text()).toBe(200);
  const body = (await result.json()) as { result: { outcome: string } };
  expect(body.result.outcome).toBe("verified_displayed");

  await expect.poll(storedEqualsDisplayed, { timeout: 20_000 }).toBe(true);

  await page.reload();
  // Overview waits on a live Open-Meteo read before it renders, so give the
  // first paint after a reload room for a slow external source.
  await expect(page.locator('[data-badge="displayed"]').first()).toBeVisible({
    timeout: 40_000,
  });
  await shot(page, "overview-displayed", info.project.name);
});
