import { defineConfig, devices } from "@playwright/test";
import os from "node:os";
import path from "node:path";
import { E2E_DIST_DIR, preserveGeneratedFiles } from "./tools/nextDistIsolation";

/**
 * Browser QA on three form factors.
 *
 * The suite runs against a production build served on its own loopback port,
 * with its own throwaway data root and its own build directory, so it can
 * never touch the real ~/.note4c-control-tower, the `.next` a running tower is
 * serving from, or any other project's state. The device is always the in-repo
 * mock.
 */

export const E2E_PORT = 8656;
export const E2E_BASE_URL = `http://127.0.0.1:${E2E_PORT}`;
export const E2E_DATA_DIR = path.join(os.tmpdir(), "note4c-tower-e2e");
export const E2E_PASSPHRASE = "playwright tower passphrase";

// The suite builds into E2E_DIST_DIR, not `.next`. See tools/nextDistIsolation.ts
// for why, and for what the calls around the run are protecting.
//
// Before the web server starts, and only in the runner process: the workers
// load this same file, by which time `next build` has already rewritten the
// two generated files, and a worker that "preserved" those would preserve the
// damage. Playwright sets TEST_WORKER_INDEX in workers and nowhere else.
if (process.env.TEST_WORKER_INDEX === undefined) {
  preserveGeneratedFiles(E2E_DIST_DIR);
}

export default defineConfig({
  testDir: "tests/e2e",
  globalSetup: "./tests/e2e/global-setup.ts",
  globalTeardown: "./tests/e2e/global-teardown.ts",
  outputDir: "test-results/e2e",
  fullyParallel: false,
  workers: 1,
  forbidOnly: !!process.env.CI,
  retries: 0,
  timeout: 60_000,
  expect: { timeout: 10_000 },
  reporter: [["list"], ["html", { open: "never", outputFolder: "playwright-report" }]],

  use: {
    baseURL: E2E_BASE_URL,
    trace: "retain-on-failure",
    screenshot: "only-on-failure",
    actionTimeout: 15_000,
  },

  projects: [
    {
      name: "desktop-chromium",
      use: { ...devices["Desktop Chrome"], viewport: { width: 1440, height: 900 } },
    },
    {
      /**
       * 375x812, the reference phone for this refit.
       *
       * The width is the point: 375 px is the narrowest phone still in wide
       * use, and it replaced 390 px because those extra 15 px were exactly
       * enough to hide the overflow a 375 px screen shows. The height moved
       * from 667 to 812 for the same kind of reason in the other direction —
       * 812 is the tall-phone viewport the layout is actually lived in, and it
       * is what the above-the-fold assertions (the designer stage reachable
       * without scrolling, the sticky action bar clear of the tab strip) are
       * measured against.
       */
      name: "mobile-chromium",
      use: {
        ...devices["Desktop Chrome"],
        viewport: { width: 375, height: 812 },
        isMobile: false,
        hasTouch: true,
        deviceScaleFactor: 3,
      },
    },
    {
      /**
       * A tablet in portrait, which is the awkward width: too wide for the
       * phone layout, too narrow for the two-column designer. Only the nav and
       * the designer run here — the rest of the suite is about behaviour that
       * does not change with the viewport, and running it a third time would
       * add minutes for nothing.
       */
      name: "tablet-chromium",
      testMatch: /(02-nav|04-designer|13-mobile-nav)\.spec\.ts/,
      use: {
        ...devices["Desktop Chrome"],
        viewport: { width: 768, height: 1024 },
        isMobile: false,
        hasTouch: true,
        deviceScaleFactor: 2,
      },
    },
  ],

  webServer: {
    // A production build, then `next start` — not `next dev`. The development
    // server compiles on demand and reloads open pages when it cannot patch a
    // module graph in place ("Fast Refresh had to perform a full reload"),
    // which is a page reload landing in the middle of whatever spec happens to
    // be running. tools/e2eServer.ts has the full account. The cost is one
    // build at the head of the run; the return is a server that cannot change
    // underneath a test.
    command: `npx tsx tools/e2eServer.ts --hostname 127.0.0.1 --port ${E2E_PORT}`,
    url: `${E2E_BASE_URL}/api/auth/state`,
    reuseExistingServer: false,
    // Long enough for a cold `next build` (a warm one, reusing the cache in
    // the scratch dist tree, is a fraction of that) plus the server start.
    timeout: 300_000,
    stdout: "ignore",
    stderr: "pipe",
    env: {
      NOTE4C_TOWER_DATA_DIR: E2E_DATA_DIR,
      // Build somewhere of our own. Without this the suite builds into
      // `.next`, which is what a running `npm start` is serving the household's
      // dashboard out of. tools/nextDistIsolation.ts has the full reasoning and
      // owns putting next-env.d.ts and tsconfig.json back afterwards. Both
      // phases read it: `next build` writes there, and `next start` serves
      // whatever `distDir` resolves to, so the two must agree.
      NEXT_DIST_DIR: E2E_DIST_DIR,
      // A short simulated panel cycle keeps the badge-transition spec quick.
      NOTE4C_MOCK_PANEL_MS: "700",
      // Unlocks the test-only mock control route. Nothing else sets this.
      NOTE4C_TOWER_E2E: "1",
      // The full 390 s worst case is exercised in the unit suite; here a
      // shorter budget keeps the whole run inside a few minutes.
      NOTE4C_PUSH_MAX_WAIT_MS: "12000",

      // Every source, explicitly off.
      //
      // Next loads .env.local into the server process, and a developer's
      // .env.local is exactly where a real calendar path, a real Home
      // Assistant file and a real set of coordinates live. NOTE4C_TOWER_E2E
      // already swaps the adapters for fixtures, but belt and braces: a suite
      // about the user interface must not depend on, or reach, anything on the
      // machine that runs it. Setting these to empty is what "not configured"
      // means everywhere else in this codebase.
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
      // Fixed, so a rendered hour label does not depend on the operator's zone.
      NOTE4C_PANEL_TIMEZONE: "America/Toronto",
    },
  },
});
