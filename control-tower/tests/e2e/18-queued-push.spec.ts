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
 * A push to a device that was not listening.
 *
 * The regression this covers: a push to a panel in automatic power saving was
 * written to the ledger as `failed / unreachable` and thrown away. Nobody had
 * refused anything — the radio was off, which is what that device does most of
 * the time — and the frame the user asked for was simply gone.
 *
 * Everything here runs against the in-repo mock with `asleep` set, which
 * destroys the socket exactly as a device with no radio on does. The mock is
 * not a sleeping device and the interface says so in as many words; what is
 * being checked here is the reachability machinery, which is the same either
 * way. The real-device wording — the next expected wake, and the note that
 * nothing on the network can bring it forward — is pinned in
 * tests/unit/pushQueue.test.ts, where a real device's power block can be
 * driven directly.
 */

interface StatusPayload {
  blocking: { pushId: string; state: string } | null;
}

async function blockingPush(page: Page): Promise<StatusPayload["blocking"]> {
  const response = await page.request.get("/api/device/status");
  expect(response.status()).toBe(200);
  return ((await response.json()) as StatusPayload).blocking;
}

/**
 * Leave nothing behind. A queued push blocks every later push, so a spec that
 * abandoned one would disable the push button for every spec file after it.
 */
async function withdrawAnyQueuedPush(page: Page): Promise<void> {
  const blocking = await blockingPush(page);
  if (blocking?.state !== "queued") return;
  const csrf = await csrfFrom(page);
  await page.request.post(`/api/device/push/${blocking.pushId}`, {
    headers: { "content-type": "application/json", "x-csrf-token": csrf },
    data: { action: "cancel" },
  });
}

/** Select a dashboard with pixels no other spec has pushed, and confirm it. */
async function selectFreshDashboard(page: Page, name: string): Promise<string> {
  const { id, title } = await createDistinctDashboard(page, name);
  await selectDashboard(page, id);
  return title;
}

/**
 * Send the selection, and leave the dialog on whichever verdict it reached.
 *
 * Returns the testid suffix of the screen it ended on — "queued", "done",
 * "failed" — so a caller can assert the outcome without knowing the wording.
 * Each verdict is a screen of its own rather than one banner with the word
 * swapped, because "held, nothing sent" and "the device confirmed it" are not
 * variations on a theme.
 */
async function sendSelection(page: Page): Promise<string> {
  await page.goto("/overview");
  await expect(page.getByTestId("push-button")).toBeEnabled();
  await page.getByTestId("push-button").click();
  await expect(page.getByTestId("send-review")).toBeVisible();
  await page.getByTestId("send-confirm").click();

  /*
   * Wait for whichever verdict lands, then name it — in that order.
   *
   * `isVisible()` does not retry, so asking each screen in turn while the POST
   * is still in flight answers "no" five times in a few milliseconds. The wait
   * has to be on the set.
   */
  const outcomes = ["queued", "done", "dedup", "failed", "uncertain", "blocked"];
  await page
    .locator(outcomes.map((o) => `[data-testid="send-${o}"]`).join(", "))
    .first()
    .waitFor({ state: "visible", timeout: 60_000 });

  for (const outcome of outcomes) {
    if (await page.getByTestId(`send-${outcome}`).isVisible()) return outcome;
  }
  throw new Error("The send dialog reached no verdict");
}

/** Close whatever verdict the dialog is showing and get back to the page. */
async function closeSend(page: Page): Promise<void> {
  const close = page.getByTestId("send-flow-close");
  if (await close.isVisible().catch(() => false)) await close.click();
  await expect(page.getByTestId("send-flow")).toHaveCount(0);
}

test.beforeEach(async ({ page }) => {
  await signIn(page);
  await setMock(page, {
    reset: true,
    panelDelayMs: 50,
    asleep: false,
    unpair: false,
  });
});

test.afterEach(async ({ page }) => {
  await setMock(page, { asleep: false, unpair: false }).catch(() => undefined);
  await withdrawAnyQueuedPush(page).catch(() => undefined);
});

test("a push to a device that is not answering is queued, not lost", async ({
  page,
}, info) => {
  await selectFreshDashboard(page, `Queued ${info.project.name}`);
  await setMock(page, { asleep: true });

  expect(await sendSelection(page)).toBe("queued");
  // And the dialog says so in the same words the page will: nothing was sent,
  // and the only immediate wake is the button on the device.
  await expect(page.getByTestId("send-queued")).toContainText("Nothing has been sent");
  await closeSend(page);

  // The card is deliberately not an error. Nothing failed: the frame is
  // rendered, recorded, and waiting.
  const banner = page.getByTestId("queued-banner");
  await expect(banner).toBeVisible();
  await expect(banner).toContainText("Nothing has been sent to the device");
  await expect(banner).toContainText("held here");
  // No new send may stack on top of it. Pressing Show a composition reaches a
  // screen that says why and points at the one control that resolves it,
  // rather than a dead button with no account of itself.
  await page.getByTestId("push-button").click();
  await expect(page.getByTestId("send-queued-blocked")).toContainText(
    "already queued",
  );
  await closeSend(page);
  await shot(page, "overview-queued", info.project.name);

  // Nothing reached the panel.
  const mock = await setMock(page, {});
  expect(mock.framePuts).toBe(0);
  expect(mock.snapshot.stored.seq).toBe(0);

  // And it is durable, not a thing the page is remembering: the tower answers
  // the same after a full reload, from the ledger and the frame on disk.
  await page.reload();
  await expect(page.getByTestId("queued-banner")).toBeVisible();
  expect((await blockingPush(page))?.state).toBe("queued");
});

test("the background pass delivers it exactly once when the device answers", async ({
  page,
}, info) => {
  await selectFreshDashboard(page, `Deliver ${info.project.name}`);
  await setMock(page, { asleep: true });
  expect(await sendSelection(page)).toBe("queued");
  await closeSend(page);

  // The device wakes on its own schedule, and the scheduler's device pass —
  // the same function the LaunchAgent runs on its timer — notices. Nothing
  // here woke it, and nothing here claims to have.
  const delivered = await setMock(page, { asleep: false, runDeviceTick: true });
  expect(delivered.deviceTick).toMatchObject({
    queued: "verified_displayed",
    reachable: true,
  });
  // Exactly one frame write reached the panel for one queued push.
  expect(delivered.framePuts).toBe(1);

  // A second pass finds nothing to do and writes nothing further: the queue is
  // empty and the push is resolved.
  const again = await setMock(page, { runDeviceTick: true });
  expect(again.deviceTick).toMatchObject({ queued: "none" });
  expect(again.framePuts).toBe(1);

  await page.goto("/overview");
  await expect(page.getByTestId("queued-banner")).toBeHidden();
  await expect(page.locator('[data-badge="displayed"]').first()).toBeVisible({
    timeout: 40_000,
  });
  await expect(page.getByTestId("push-button")).toBeEnabled();
  await shot(page, "overview-queued-delivered", info.project.name);
});

test("the queued frame can be sent by hand once somebody wakes the panel", async ({
  page,
}, info) => {
  await selectFreshDashboard(page, `Send now ${info.project.name}`);
  await setMock(page, { asleep: true });
  expect(await sendSelection(page)).toBe("queued");
  await closeSend(page);

  // Trying while it is still away changes nothing and says so — it does not
  // fail the push, and it does not pretend to have woken anything.
  await page.getByTestId("deliver-queued").click();
  await expect(page.getByTestId("toast")).toContainText("waiting");
  await expect(page.getByTestId("queued-banner")).toBeVisible();
  expect((await setMock(page, {})).framePuts).toBe(0);

  // Somebody presses the button on the device. That is the only wake there is.
  await setMock(page, { asleep: false });
  await page.reload();
  await page.getByTestId("deliver-queued").click();
  await expect(page.getByTestId("toast")).toContainText("verified displayed", {
    timeout: 45_000,
  });
  expect((await setMock(page, {})).framePuts).toBe(1);
  await expect(page.getByTestId("queued-banner")).toBeHidden();
});

test("a device that answers and refuses fails the queued push, and stops", async ({
  page,
}, info) => {
  await selectFreshDashboard(page, `Refused ${info.project.name}`);
  await setMock(page, { asleep: true });
  expect(await sendSelection(page)).toBe("queued");
  await closeSend(page);

  // The panel is awake, and has been re-paired with something else: every
  // authenticated call is refused. That is a device saying no, not a device
  // being away, and it must not be retried until a six-hour timer runs out.
  const refused = await setMock(page, {
    asleep: false,
    unpair: true,
    runDeviceTick: true,
  });
  expect(refused.deviceTick).toMatchObject({ queued: "failed" });

  // A second pass has nothing left to try: the failure is terminal and the
  // tower is no longer holding the bytes.
  const again = await setMock(page, { runDeviceTick: true });
  expect(again.deviceTick).toMatchObject({ queued: "none" });
  expect(await blockingPush(page)).toBeNull();

  await setMock(page, { unpair: false });
  await page.goto("/overview");
  await expect(page.getByTestId("queued-banner")).toBeHidden();
  await expect(page.locator('[data-badge="failed"]').first()).toBeVisible();
  // And the tower is usable again: a bounded failure, not a stuck queue.
  await expect(page.getByTestId("push-button")).toBeEnabled();
  await shot(page, "overview-queued-refused", info.project.name);
});

test("a queued push can be withdrawn, and nothing was ever sent", async ({
  page,
}, info) => {
  await selectFreshDashboard(page, `Withdraw ${info.project.name}`);
  await setMock(page, { asleep: true });
  expect(await sendSelection(page)).toBe("queued");
  await closeSend(page);

  await page.getByTestId("cancel-queued").click();
  await expect(page.getByTestId("toast")).toContainText("nothing was sent");
  await expect(page.getByTestId("queued-banner")).toBeHidden();
  await expect(page.getByTestId("push-button")).toBeEnabled();

  await setMock(page, { asleep: false });
  const mock = await setMock(page, { runDeviceTick: true });
  // Withdrawn means withdrawn: a later window does not resurrect it.
  expect(mock.deviceTick).toMatchObject({ queued: "none" });
  expect(mock.framePuts).toBe(0);
  expect(await blockingPush(page)).toBeNull();
});
