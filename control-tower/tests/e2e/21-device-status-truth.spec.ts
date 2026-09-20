import { expect, test, type Route } from "@playwright/test";
import { signIn } from "./helpers";

/**
 * The Device page, driven through the exact sequence the production captures
 * show — in a browser, against crafted answers from the tower's own route.
 *
 * The device in this suite is always the in-repo mock, and a mock has no sleep
 * to be in, so it cannot produce the case that matters: a *real* panel that did
 * not answer one read. The answers are therefore stubbed at the route, which is
 * the seam the page actually consumes, and their shape is copied from the
 * production install: a failed read whose last confirmed reading is the
 * interactive window the panel reported a minute earlier, then the same panel
 * answering `awake: true`.
 *
 * What is being pinned is the thing the captures got wrong. One failed read
 * produced a badge reading "Asleep" — "the designed state between refreshes" —
 * directly above a red UNREACHABLE banner, for a panel that was awake and
 * answering other clients on the same LAN in 55 ms.
 */

/**
 * When the panel last answered, relative to the moment the test runs.
 *
 * A minute ago, holding an interactive window it reported as having fifteen
 * minutes left: the state immediately after somebody presses BOOT and walks
 * back to the laptop, which is the moment the production captures were taken
 * in. A fixed timestamp would have the window expired by the time the suite
 * runs and would test a different branch.
 */
const SEEN_AT = new Date(Date.now() - 60_000).toISOString();

const INTERACTIVE_POWER = {
  contract: 1,
  mode: "interactive",
  desired_mode: "interactive",
  ack: "acknowledged",
  awake: true,
  sleep_intent: false,
  interactive_remaining_s: 900,
  wake_interval_min: 60,
  timer_armed: false,
  next_wake_in_s: 0,
  next_wake_epoch: null,
  last_wake_reason: "button",
  last_outcome: null,
  budget_exhausted_phase: null,
  consecutive_failures: 0,
  battery: { present: true, calibrated: true, plausible: true, mv: 3937, percent: 80 },
  charge: { state: "no_power", charging: false },
};

/** What the tower answers while this machine is refusing the connection. */
function heldOffPayload(now: Date) {
  return {
    reachable: false,
    power: null,
    powerSupported: null,
    powerIntent: null,
    powerIntentDisposition: "none",
    powerIntentDetail: "",
    batteryUnavailableReason: null,
    reachabilityNote: "Not answering.",
    noRemoteWake: "Wi-Fi cannot wake it.",
    deviceLastSeenAt: SEEN_AT,
    nextWake: {
      at: new Date(Date.parse(SEEN_AT) + 60 * 60_000).toISOString(),
      estimated: true,
    },
    simulated: false,
    tokenConfigured: true,
    deviceMode: "real",
    deviceAddress: "192.168.0.60",
    readAt: now.toISOString(),
    detail:
      "This machine refused the connection to 192.168.0.60:80 from its own routing table, in 0 ms (EHOSTUNREACH): it has no current address-resolution entry for it and will not try again for a few seconds. Nothing was sent, and nothing was learned about the panel.",
    failure: {
      kind: "transport",
      code: "unreachable",
      errno: "EHOSTUNREACH",
      heldLocally: true,
      deviceAnswered: false,
      notable: true,
      detail: "",
    },
    blocking: null,
    lastPush: null,
    manual: false,
    lastConfirmed: { at: SEEN_AT, power: INTERACTIVE_POWER, powerSupported: true },
  };
}

/** And what it answers once the panel is reached. Taken from the live read. */
function awakePayload(now: Date) {
  return {
    reachable: true,
    power: { ...INTERACTIVE_POWER, interactive_remaining_s: 76 },
    powerSupported: true,
    powerIntent: null,
    powerIntentDisposition: "none",
    powerIntentDetail: "",
    deviceLastSeenAt: SEEN_AT,
    batteryUnavailableReason: null,
    reachabilityNote: "Awake for about 2 more minutes, then back to power saving on its own.",
    noRemoteWake: "Wi-Fi cannot wake it.",
    nextWake: null,
    simulated: false,
    tokenConfigured: true,
    deviceMode: "real",
    deviceAddress: "192.168.0.60",
    readAt: now.toISOString(),
    status: {
      firmware: "Marvin 0.2",
      api: 2,
      capabilities: ["power.hybrid.v1"],
      provisioned: true,
      lockdown: false,
      config_revision: 12,
      stored: { present: true, seq: 110, sha256: "a".repeat(64) },
      displayed: { present: true, seq: 110, sha256: "a".repeat(64) },
    },
    displayedMatch: null,
    storedMatch: null,
    storedEqualsDisplayed: true,
    blocking: null,
    lastPush: null,
    manual: true,
    lastConfirmed: {
      at: now.toISOString(),
      power: { ...INTERACTIVE_POWER, interactive_remaining_s: 76 },
      powerSupported: true,
    },
  };
}

const CONFIG_PAYLOAD = {
  reachable: false,
  simulated: false,
  supported: true,
  detail: "The device was not reached for its configuration.",
};

async function json(route: Route, body: unknown): Promise<void> {
  await route.fulfill({
    status: 200,
    contentType: "application/json",
    headers: { "cache-control": "no-store" },
    body: JSON.stringify(body),
  });
}

test("a failed read never claims the panel is asleep, and a good one clears it", async ({
  page,
}) => {
  await signIn(page);

  const statusUrls: string[] = [];
  let reachable = false;

  await page.route("**/api/device/config*", (route) => json(route, CONFIG_PAYLOAD));
  await page.route("**/api/device/status*", async (route) => {
    statusUrls.push(route.request().url());
    const now = new Date();
    await json(route, reachable ? awakePayload(now) : heldOffPayload(now));
  });

  await page.goto("/device");

  const strip = page.getByTestId("state-strip");

  // 1. The failed read. The panel's own last word was an interactive window
  //    with fifteen minutes in it, so the tower says it should be answering —
  //    and it says when it last answered rather than inventing a state.
  await expect(strip).toHaveAttribute("data-state", "unreachable");
  await expect(page.getByTestId("state-strip-badge")).toHaveText("Unreachable");
  await expect(strip).not.toContainText("designed state between refreshes");
  await expect(strip).toContainText("It last answered at");

  // The banner underneath is about the same read, and no longer contradicts it.
  await expect(page.getByTestId("device-unreachable-detail")).toContainText(
    "own routing table",
  );
  await expect(page.getByTestId("device-unreachable-detail")).toContainText(
    "nothing was learned about the panel",
  );

  // The header chip, which is a second rendering of the same reading.
  await expect(page.getByTestId("device-chip-state")).toHaveText("Unreachable");

  // 2. "Check now". It must reach the tower uncached and marked as a person
  //    asking, which is what buys the longer read-only retry budget.
  reachable = true;
  const before = statusUrls.length;
  await page.getByTestId("refresh").click();
  await expect
    .poll(() => statusUrls.length, { timeout: 15_000 })
    .toBeGreaterThan(before);
  expect(statusUrls.at(-1)).toContain("force=1");

  // 3. The panel answered `awake: true`, so the page says so immediately —
  //    badge, chip and banner all at once.
  await expect(strip).toHaveAttribute("data-state", "awake");
  await expect(page.getByTestId("state-strip-badge")).toHaveText("Awake");
  await expect(page.getByTestId("device-chip-state")).toHaveText("Awake");
  await expect(page.getByTestId("device-unreachable")).toHaveCount(0);
  // And the sentence from the failed read is gone from the page entirely.
  await expect(page.locator("body")).not.toContainText("EHOSTUNREACH");
});

test("a panel that answers and reports it is not awake reads ASLEEP", async ({
  page,
}) => {
  await signIn(page);
  await page.route("**/api/device/config*", (route) => json(route, CONFIG_PAYLOAD));
  await page.route("**/api/device/status*", async (route) => {
    const now = new Date();
    const payload = awakePayload(now);
    payload.power = { ...payload.power, awake: false, sleep_intent: true };
    await json(route, payload);
  });

  await page.goto("/device");
  await expect(page.getByTestId("state-strip")).toHaveAttribute(
    "data-state",
    "asleep",
  );
  await expect(page.getByTestId("state-strip-badge")).toHaveText("Asleep");
});
