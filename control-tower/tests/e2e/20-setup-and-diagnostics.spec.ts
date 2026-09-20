import { expect, test } from "@playwright/test";
import { setMock, shot, signIn } from "./helpers";

test.beforeEach(async ({ page }) => {
  await signIn(page);
  await setMock(page, { asleep: false }).catch(() => undefined);
});

/**
 * The setup wizard.
 *
 * What it must not do is the thing the prototype it was drawn from does: offer
 * a "pair now" button for a two-minute window. The device really does have one
 * — `POST /api/v1/dashboard/pair` claims a token during a window somebody
 * opens by pressing buttons on the device — but this tower has no client for
 * it, and re-pairing would mint a new token and invalidate the credential
 * anything else on the machine is already using. So the step describes the
 * mechanism that exists, and this spec is the thing that stops a future edit
 * quietly adding a button with nothing behind it.
 */
test("setup describes the pairing that exists, and offers no button for one that does not", async ({
  page,
}, info) => {
  await page.goto("/device");
  await page.getByTestId("open-setup").click();
  await page.waitForURL("**/device/setup");

  await expect(page.getByRole("heading", { name: "Set up the connection" })).toBeVisible();
  const steps = page.getByTestId("setup-steps");
  await expect(steps).toBeVisible();

  // The passphrase step is done, because it is: signing in required it.
  await expect(page.getByTestId("setup-step-1")).toHaveAttribute("data-state", "done");

  // It never claims a pairing window the tower cannot open, and it says where
  // pairing actually happens whichever step is live — not folded inside the
  // step nobody has reached yet.
  const page_text = (await page.locator("main").textContent()) ?? "";
  expect(page_text).not.toMatch(/2-minute window|two-minute window/i);
  expect(page_text).toMatch(/Pairing happens on the device/);

  await shot(page, "setup", info.project.name);
});

test("setup resumes from what the tower reports, not from what the page remembers", async ({
  page,
}) => {
  await page.goto("/device/setup");
  const active = page.locator('[data-state="active"]');
  await expect(active).toHaveCount(1);
  const before = await active.getAttribute("data-testid");

  // A reload is the cheapest proof that the step is derived rather than held:
  // a wizard with its own counter would come back at step one.
  await page.reload();
  await expect(page.locator('[data-state="active"]')).toHaveAttribute(
    "data-testid",
    before ?? "",
  );
});

test("the address step records the address and reports what answered", async ({
  page,
}) => {
  await page.goto("/device/setup");
  const field = page.getByTestId("setup-address");

  // Only reachable while step two is the live one, which it is on a tower
  // pointed at the mock: no address has been set for a real panel.
  if (await field.isVisible().catch(() => false)) {
    await field.fill("192.168.4.4");
    await page.getByTestId("setup-check-address").click();
    const note = page.getByTestId("setup-address-note");
    await expect(note).toBeVisible({ timeout: 20_000 });
    // Either answer is honest. What it must never do is report a silent device
    // as a wrong address: a sleeping panel looks exactly the same from here.
    await expect(note).toContainText(/answered|did not answer/);
    if (await note.textContent().then((t) => (t ?? "").includes("did not answer"))) {
      await expect(note).toContainText("not proof the address is wrong");
    }
  }
});

/**
 * Diagnostics, as four tabs rather than nine stacked cards.
 *
 * Every tab carries its own footnote inside the box it belongs to, because the
 * caveat is part of the evidence: "this is a log, not an archive" is useless
 * three cards away from the log.
 */
test("diagnostics separates events, device facts, history and the tower", async ({
  page,
}, info) => {
  await page.goto("/diagnostics");
  await expect(page.getByText("Nothing here is needed day to day")).toBeVisible();

  // Events is the tab it opens on: what happened, most recent first.
  await expect(page.getByTestId("audit-table")).toBeVisible();
  await expect(page.getByTestId("audit-table")).toContainText("log, not an archive");

  await page.getByTestId("diag-tab-facts").click();
  const facts = page.getByTestId("device-facts");
  await expect(facts).toBeVisible();
  await expect(facts).toContainText("400 × 300, four colours");
  // The phrase that makes the whole card trustworthy: a value the device did
  // not send is said to be missing rather than left blank.
  await expect(facts).toContainText("reported by the device itself");
  await expect(facts).toContainText("means exactly that");

  await page.getByTestId("diag-tab-history").click();
  await expect(page.getByTestId("ledger-table")).toBeVisible();
  await expect(page.getByTestId("ledger-table")).toContainText(
    "stays “uncertain” until a human resolves it",
  );

  await page.getByTestId("diag-tab-tower").click();
  await expect(page.getByTestId("tower-version")).toBeVisible();
  await expect(page.getByTestId("bind-verdict")).toBeVisible();
  // The font provenance stayed with the tower rather than being dropped when
  // the page was reorganised: it is what makes the atlases redistributable.
  await expect(page.getByTestId("font-upstream-inter")).toBeVisible();
  // And the faces the browser page itself is set in, which is a different
  // question from which faces the panel can be set in, and which is what a
  // reader checking "nothing comes from a font CDN" actually wants.
  await expect(page.getByTestId("interface-faces")).toContainText("Space Grotesk");

  await shot(page, "diagnostics-tabs", info.project.name);
});

/**
 * A device fact the tower could not read says so.
 *
 * The mock can be silenced, which is the only way to see this page with no
 * device behind it. What must not happen is blank rows: "not reported" is a
 * different fact from "not asked", and the page has to keep them apart.
 */
test("device facts say “not reported” rather than going blank", async ({ page }) => {
  await setMock(page, { asleep: true });
  await page.goto("/diagnostics");
  await page.getByTestId("diag-tab-facts").click();

  const facts = page.getByTestId("device-facts");
  await expect(facts).toBeVisible();
  await expect(facts).toContainText("not reported");
  // The panel geometry is a fact about the hardware this product is for, not
  // something the device has to be awake to tell us.
  await expect(facts).toContainText("400 × 300, four colours");

  await setMock(page, { asleep: false });
});
