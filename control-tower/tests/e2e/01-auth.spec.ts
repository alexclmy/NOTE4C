import { expect, test } from "@playwright/test";
import { E2E_PASSPHRASE } from "../../playwright.config";
import { completeFirstRun, resetDataRoot, setMock, shot } from "./helpers";

/**
 * Runs first in every project, and is the only spec that resets state. Each
 * project therefore starts from a genuine first run against a device that has
 * never been written to, rather than inheriting whatever the previous project
 * left behind.
 */
test.describe.configure({ mode: "serial" });

test("first run sets a passphrase and lands on Overview", async ({ page }, info) => {
  resetDataRoot();

  await page.goto("/login");
  await shot(page, "login", info.project.name);

  const welcome = page.getByRole("heading", { name: "Welcome" });
  if (await welcome.isVisible().catch(() => false)) {
    await completeFirstRun(page);
  } else {
    await page.getByTestId("passphrase").fill(E2E_PASSPHRASE);
    await page.getByTestId("submit-auth").click();
    await page.waitForURL("**/overview");
  }

  await expect(page.getByRole("heading", { name: "Overview" })).toBeVisible();
  await expect(page.getByTestId("simulated-banner")).toBeVisible();

  // The tower store is new, so give the simulated panel a matching blank slate.
  await setMock(page, { reset: true, failAck: false, panelDelayMs: 700 });
  await page.reload();
  await expect(page.getByText("Nothing is stored on the device yet")).toBeVisible();
});

test("an unauthenticated visitor is redirected away from every page", async ({
  browser,
}) => {
  const context = await browser.newContext();
  const page = await context.newPage();
  for (const path of [
    "/overview",
    "/dashboards",
    "/device",
    "/voice",
    "/diagnostics",
  ]) {
    await page.goto(path);
    await page.waitForURL("**/login");
    await expect(page.getByRole("heading", { name: /Welcome|Sign in/ })).toBeVisible();
  }
  await context.close();
});

test("the API refuses reads and mutations without a session", async ({ request }) => {
  expect((await request.get("/api/dashboards")).status()).toBe(401);
  expect((await request.get("/api/overview")).status()).toBe(401);
  expect((await request.get("/api/diagnostics")).status()).toBe(401);

  const created = await request.post("/api/dashboards", {
    data: { title: "Should not exist" },
  });
  expect(created.status()).toBe(401);
});

test("a mutation without the CSRF header is refused even with a session", async ({
  page,
  request,
}) => {
  await page.goto("/login");
  const welcome = page.getByRole("heading", { name: "Welcome" });
  if (await welcome.isVisible().catch(() => false)) {
    await completeFirstRun(page);
  } else {
    await page.getByTestId("passphrase").fill(E2E_PASSPHRASE);
    await page.getByTestId("submit-auth").click();
    await page.waitForURL("**/overview");
  }

  const cookies = await page.context().cookies();
  const session = cookies.find((c) => c.name === "note4c_tower_session");
  expect(session).toBeDefined();
  expect(session?.httpOnly).toBe(true);
  expect(session?.sameSite).toBe("Strict");

  const csrf = cookies.find((c) => c.name === "note4c_tower_csrf");
  expect(csrf).toBeDefined();
  // Readable by the page on purpose: that is the double-submit mechanism.
  expect(csrf?.httpOnly).toBe(false);

  const cookieHeader = cookies.map((c) => `${c.name}=${c.value}`).join("; ");

  const withoutHeader = await request.post("/api/dashboards", {
    headers: { cookie: cookieHeader, "content-type": "application/json" },
    data: { title: "No CSRF" },
  });
  expect(withoutHeader.status()).toBe(403);

  const wrongHeader = await request.post("/api/dashboards", {
    headers: {
      cookie: cookieHeader,
      "content-type": "application/json",
      "x-csrf-token": "not-the-token",
    },
    data: { title: "Wrong CSRF" },
  });
  expect(wrongHeader.status()).toBe(403);

  const correct = await request.post("/api/dashboards", {
    headers: {
      cookie: cookieHeader,
      "content-type": "application/json",
      "x-csrf-token": csrf?.value ?? "",
    },
    data: { title: "CSRF accepted" },
  });
  expect(correct.status()).toBe(201);
});

test("signing out ends the session", async ({ page }) => {
  await page.goto("/login");
  const welcome = page.getByRole("heading", { name: "Welcome" });
  if (await welcome.isVisible().catch(() => false)) {
    await completeFirstRun(page);
  } else {
    await page.getByTestId("passphrase").fill(E2E_PASSPHRASE);
    await page.getByTestId("submit-auth").click();
    await page.waitForURL("**/overview");
  }

  // The rail is hidden on the mobile viewport, so drive the API the button uses.
  await page.evaluate(async () => {
    const token = document.cookie
      .split(";")
      .map((p) => p.trim())
      .find((p) => p.startsWith("note4c_tower_csrf="))
      ?.slice("note4c_tower_csrf=".length);
    await fetch("/api/auth/logout", {
      method: "POST",
      headers: { "x-csrf-token": decodeURIComponent(token ?? "") },
    });
  });

  await page.goto("/overview");
  await page.waitForURL("**/login");
  await expect(page.getByRole("heading", { name: "Sign in" })).toBeVisible();
});
