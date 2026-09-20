import { defineConfig } from "vitest/config";
import { fileURLToPath } from "node:url";

export default defineConfig({
  test: {
    environment: "node",
    /**
     * A fixed panel timezone for the whole suite.
     *
     * The panel's timezone now defaults to the machine's own, which is right
     * for a product installed on one computer and wrong for a test suite: the
     * hour labels on a forecast, the day a countdown lands on and the semantic
     * hash that decides whether a frame is a duplicate all move with it, so
     * the same assertions would pass in Toronto and fail in Paris. Pinning it
     * here keeps those tests about the renderer rather than about whoever ran
     * them. Tests that are about the resolution itself set and unset the
     * variable themselves.
     *
     * WHICH zone is arbitrary, and worth saying out loud because this is the
     * one machine-specific-looking value the repository still contains: it is
     * a fixed clock for the suite, not a statement about where this software
     * is installed or where anyone lives. It is not a default, it is not read
     * by the product, and `.env.example` documents `Etc/UTC`. Changing it is a
     * mechanical but real job — several assertions encode its UTC offset and
     * its DST dates, and two golden digests draw a timestamp in it — so it was
     * left alone deliberately rather than by oversight.
     */
    env: { NOTE4C_PANEL_TIMEZONE: "America/Toronto" },
    include: ["tests/unit/**/*.test.ts"],
    globals: false,
    pool: "forks",
  },
  resolve: {
    alias: {
      "@": fileURLToPath(new URL("./src", import.meta.url)),
      "@mock": fileURLToPath(new URL("./mock-device", import.meta.url)),
    },
  },
});
