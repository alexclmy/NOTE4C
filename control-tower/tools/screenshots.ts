/**
 * The screenshots in docs/images/, made reproducibly.
 *
 * Run by hand, committed as PNGs:
 *
 *     npx tsx tools/screenshots.ts
 *
 * Why a script rather than "take a screenshot when you remember to":
 *
 *  - **It must never photograph somebody's home.** This drives a tower with
 *    its own throwaway data root, pointed at the in-repo mock, with source
 *    fixtures on (NOTE4C_TOWER_E2E=1). No weather for a real place, no
 *    calendar, no Reminders, no LAN address — nothing on the resulting images
 *    is a fact about the machine that produced them.
 *  - **The same content every time.** It seeds one dashboard with fixed
 *    wording, so a re-run produces the same picture and a diff in
 *    docs/images/ means the interface changed, not that the weather did.
 *  - **Both form factors.** 1440x900 and 375x812, the two the README shows and
 *    the two Playwright measures.
 *
 * It starts its own development server, on a port of its own and building into
 * a directory of its own, so it cannot disturb a tower you have running.
 *
 * Every page is also inspected while it is on screen, because a screenshot run
 * already loads each page at each width and a picture does not show a thrown
 * exception, a console error or a font fetched from somebody else's CDN. The
 * four properties are in `Problem` below; what was seen is written to
 * test-results/screenshots-qa.json (ignored, like everything under
 * test-results/) and a run that saw anything fails after writing it.
 */

import { spawn, type ChildProcess } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { chromium, type Page } from "@playwright/test";
import {
  SCREENSHOT_DIST_DIR,
  preserveGeneratedFiles,
  restoreGeneratedFiles,
} from "./nextDistIsolation";

const PORT = 8658;
const BASE = `http://127.0.0.1:${PORT}`;
const DATA_DIR = path.join(os.tmpdir(), "note4c-tower-screenshots");
const OUT_DIR = path.join("docs", "images");
const QA_REPORT = path.join("test-results", "screenshots-qa.json");
const PASSPHRASE = "screenshot tower passphrase";

const VIEWPORTS = [
  { name: "desktop", width: 1440, height: 900 },
  { name: "mobile", width: 375, height: 812 },
] as const;

const PAGES = [
  { slug: "overview", path: "/overview" },
  { slug: "dashboards", path: "/dashboards" },
  { slug: "designer", path: null }, // resolved once a dashboard exists
  { slug: "device", path: "/device" },
  { slug: "voice", path: "/voice" },
  { slug: "diagnostics", path: "/diagnostics" },
] as const;

/** Something noticed while one page was on screen at one width. */
type Problem = {
  kind: "pageerror" | "console-error" | "external-request" | "overflow";
  detail: string;
};

/** One captured page: the picture, and what the page did while it was taken. */
type PageReport = {
  viewport: string;
  slug: string;
  url: string;
  file: string;
  viewportWidth: number;
  scrollWidth: number;
  problems: Problem[];
};

/**
 * Everything this tower needs, the tower serves.
 *
 * A page that reaches out at runtime for a font, a script or an icon is a page
 * that renders differently — or not at all — on a kitchen worktop with the
 * uplink down, and it tells a third party that this machine loaded it. So a
 * request to any origin but the dev server's is a defect, not a detail.
 * `data:`, `blob:` and `about:` are the page talking to itself.
 */
function isExternalRequest(url: string): boolean {
  if (url.startsWith(BASE)) return false;
  return !/^(data|blob|about):/.test(url);
}

/**
 * Write what was seen, then fail on it — in that order.
 *
 * The report is the point of the run as much as the PNGs are, so it is on disk
 * before anything throws; a run that failed and left no evidence would send
 * whoever reads the failure straight back to re-running it.
 *
 * Only the captured pages are failed on. `outsideCapture` holds anything the
 * login and seeding steps produced: worth reading, but those pages are not
 * photographed and not part of what this asserts.
 */
function finishQa(
  pages: PageReport[],
  outsideCapture: Array<Problem & { viewport: string }>,
): void {
  const failures = pages.flatMap((page) =>
    page.problems.map(
      (problem) =>
        `${page.viewport}/${page.slug}: ${problem.kind}: ${problem.detail}`,
    ),
  );

  fs.mkdirSync(path.dirname(QA_REPORT), { recursive: true });
  fs.writeFileSync(
    QA_REPORT,
    `${JSON.stringify(
      {
        generatedAt: new Date().toISOString(),
        base: BASE,
        viewports: VIEWPORTS,
        pages,
        outsideCapture,
        failures,
        ok: failures.length === 0,
      },
      null,
      2,
    )}\n`,
  );
  console.log(`wrote ${QA_REPORT}`);

  if (failures.length > 0) {
    throw new Error(
      `Screenshot QA found ${failures.length} problem(s):\n  ${failures.join("\n  ")}`,
    );
  }
}

async function waitForServer(): Promise<void> {
  for (let attempt = 0; attempt < 120; attempt += 1) {
    try {
      const response = await fetch(`${BASE}/api/auth/state`);
      if (response.ok) return;
    } catch {
      // Not up yet.
    }
    await new Promise((resolve) => setTimeout(resolve, 1000));
  }
  throw new Error("The screenshot server never came up");
}

async function csrf(page: Page): Promise<string> {
  const cookies = await page.context().cookies();
  return cookies.find((c) => c.name === "note4c_tower_csrf")?.value ?? "";
}

async function main(): Promise<void> {
  fs.rmSync(DATA_DIR, { recursive: true, force: true });
  fs.mkdirSync(OUT_DIR, { recursive: true });

  // Keep the two files `next dev` is about to rewrite. See nextDistIsolation.ts.
  preserveGeneratedFiles(SCREENSHOT_DIST_DIR);

  const server: ChildProcess = spawn(
    "npx",
    ["next", "dev", "--hostname", "127.0.0.1", "--port", String(PORT)],
    {
      env: {
        ...process.env,
        NOTE4C_TOWER_DATA_DIR: DATA_DIR,
        // Build somewhere of our own, so taking screenshots cannot overwrite
        // the `.next` a running tower is serving the panel out of.
        NEXT_DIST_DIR: SCREENSHOT_DIST_DIR,
        // Fixtures, not the world. See src/server/sources/e2eFixtures.ts.
        NOTE4C_TOWER_E2E: "1",
        NOTE4C_MOCK_PANEL_MS: "300",
        NOTE4C_PUSH_MAX_WAIT_MS: "12000",
        // A fixed zone, so the timestamp tile reads the same in every run.
        NOTE4C_PANEL_TIMEZONE: "UTC",
        // Every source explicitly off, so a developer's own .env.local — which
        // Next loads into this process — cannot put their calendar, their
        // sensors or their coordinates into a committed screenshot.
        NOTE4C_CALENDAR_SNAPSHOT_PATH: "",
        NOTE4C_CALENDAR_NAME: "",
        NOTE4C_REMINDCTL_BIN: "",
        NOTE4C_HA_ENV_PATH: "",
        NOTE4C_COMPOSER_ORIGIN: "",
        NOTE4C_WEATHER_LATITUDE: "",
        NOTE4C_WEATHER_LONGITUDE: "",
        NOTE4C_WEATHER_FALLBACK_URL: "",
        NOTE4C_BRIDGE_TOKEN_PATH: "",
        NOTE4C_DEVICE_ADDRESS: "",
      },
      // Inherit stderr: a server that fails to start is the one failure this
      // script cannot recover from, and swallowing its reason turns that into a
      // silent two-minute timeout.
      stdio: ["ignore", "ignore", "inherit"],
    },
  );

  try {
    await waitForServer();

    const browser = await chromium.launch();
    let dashboardId = "";
    const reports: PageReport[] = [];
    const outsideCapture: Array<Problem & { viewport: string }> = [];

    for (const viewport of VIEWPORTS) {
      const context = await browser.newContext({
        viewport: { width: viewport.width, height: viewport.height },
        deviceScaleFactor: 2,
      });
      const page = await context.newPage();

      // The page the listeners below are currently filing against. Null until
      // the first capture, because signing in and seeding a dashboard happen on
      // pages nobody photographs.
      let current: PageReport | null = null;
      const note = (problem: Problem): void => {
        if (current) current.problems.push(problem);
        else outsideCapture.push({ viewport: viewport.name, ...problem });
      };

      // Attached once per context, before anything navigates: a module that
      // throws while mounting does it before `goto` resolves, and a listener
      // added afterwards would never hear it.
      page.on("pageerror", (error) => {
        note({ kind: "pageerror", detail: error.message });
      });
      page.on("console", (message) => {
        if (message.type() === "error") {
          note({ kind: "console-error", detail: message.text() });
        }
      });
      page.on("request", (request) => {
        if (isExternalRequest(request.url())) {
          note({
            kind: "external-request",
            detail: `${request.resourceType()} ${request.url()}`,
          });
        }
      });

      // First run, once: set the passphrase; afterwards, sign in.
      //
      // Asked of the API rather than inferred from the page. The login form
      // decides which of the two it is from its own fetch, so a screenshot run
      // that looked at the rendered heading raced it, filled one field of a
      // two-field form, and then waited thirty seconds for a navigation that
      // was never going to happen.
      const authState = await page.request.get(`${BASE}/api/auth/state`);
      const { passphraseSet } = (await authState.json()) as {
        passphraseSet: boolean;
      };

      await page.goto(`${BASE}/login`);
      await page.getByTestId("passphrase").waitFor();
      await page.getByTestId("passphrase").fill(PASSPHRASE);
      if (!passphraseSet) {
        await page.getByTestId("passphrase-confirm").fill(PASSPHRASE);
      }
      await page.getByTestId("submit-auth").click();
      await page.waitForURL("**/overview");

      if (dashboardId === "") {
        await page.goto(`${BASE}/dashboards`);
        const token = await csrf(page);
        // Through the API, the way the first template does it. The gallery has
        // no title-and-Create form any more: a new composition starts from the
        // template picker, and driving a dialog to seed a screenshot would be
        // photographing the seeding rather than the product.
        const created = await page.request.post(`${BASE}/api/dashboards`, {
          headers: { "content-type": "application/json", "x-csrf-token": token },
          data: { title: "Kitchen panel", starter: true },
        });
        const body = (await created.json()) as { record: { doc: { id: string } } };
        dashboardId = body.record.doc.id;
        // Selected, so Overview has something to talk about.
        await page.request.patch(`${BASE}/api/dashboards/${dashboardId}`, {
          headers: { "content-type": "application/json", "x-csrf-token": token },
          data: { selected: true },
        });
        // One push, so the panel card shows a frame rather than an empty state.
        await page.request.post(`${BASE}/api/device/push`, {
          headers: { "content-type": "application/json", "x-csrf-token": token },
          data: { force: true },
        });
      }

      for (const target of PAGES) {
        const url =
          target.path ?? `/dashboards/${dashboardId}/edit`;
        const file = path.join(OUT_DIR, `${viewport.name}-${target.slug}.png`);
        const report: PageReport = {
          viewport: viewport.name,
          slug: target.slug,
          url,
          file,
          viewportWidth: viewport.width,
          scrollWidth: 0,
          problems: [],
        };
        reports.push(report);
        current = report;

        await page.goto(`${BASE}${url}`);
        // Let the first read land, so nothing is photographed mid-skeleton.
        await page.waitForTimeout(2500);

        report.scrollWidth = await page.evaluate(
          () => document.documentElement.scrollWidth,
        );
        // One pixel of tolerance for sub-pixel layout rounding, the same
        // allowance tests/e2e/13-mobile-nav.spec.ts makes; anything more is
        // content that does not fit the shot being taken of it.
        if (report.scrollWidth > viewport.width + 1) {
          report.problems.push({
            kind: "overflow",
            detail: `scrollWidth ${report.scrollWidth} exceeds the ${viewport.width} px viewport`,
          });
        }

        await page.screenshot({ path: file, fullPage: false });
        console.log(`wrote ${file}`);
      }

      await context.close();
    }

    await browser.close();

    // After the pictures are on disk, so a QA failure never costs the run.
    finishQa(reports, outsideCapture);
  } finally {
    server.kill("SIGTERM");
    fs.rmSync(DATA_DIR, { recursive: true, force: true });
    // After the server is gone, so nothing can rewrite them again.
    restoreGeneratedFiles(SCREENSHOT_DIST_DIR);
  }
}

main().catch((error: unknown) => {
  console.error(error);
  process.exitCode = 1;
});
