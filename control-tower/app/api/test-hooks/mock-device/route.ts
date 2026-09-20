import { NextResponse } from "next/server";
import { z } from "zod";
import { guarded, jsonError } from "@/server/auth/guard";
import { getMockDevice, mockToken } from "@/server/device/mock";
import { invalidateOverviewSnapshot } from "@/server/overview/snapshot";
import { runDeviceWindowTick } from "@/server/refreshScheduler";

export const dynamic = "force-dynamic";

/**
 * Test-only control over the simulated panel.
 *
 * Refuses unless NOTE4C_TOWER_E2E=1, which only the Playwright web server
 * sets, and it is still behind the ordinary session and CSRF gate. It can
 * reach nothing but the in-process mock: there is no path from here to real
 * hardware.
 */
const BodySchema = z.object({
  failAck: z.boolean().optional(),
  panelDelayMs: z.number().int().min(0).max(60_000).optional(),
  reset: z.boolean().optional(),
  /**
   * Reproduce somebody changing a setting on the device itself: the revision
   * moves, and the tower never saw it move. There is no other way to exercise
   * the compare-and-swap from a browser test, and the lost update it prevents
   * is the failure the Device page exists to make impossible.
   */
  bumpRevision: z.boolean().optional(),
  slideMin: z
    .union([z.literal(0), z.literal(5), z.literal(10), z.literal(30)])
    .optional(),
  /**
   * Make the mock stop answering, the way a deep-sleeping device does.
   *
   * The single most important state this product has to render honestly, and
   * until now the browser suite could not reach it: every spec ran against a
   * device that was always awake, so the asleep, pending and uncertain
   * readings on Overview and Device were only ever exercised in unit tests.
   */
  asleep: z.boolean().optional(),
  /**
   * Hold every request open for this long before answering nothing.
   *
   * The failure `asleep` cannot reach: a socket that is accepted and then
   * never answered, so the caller pays its own timeout instead of failing in a
   * millisecond. See the note on MockDevice.stallMs. Capped below the device
   * read timeout's own worst case so a spec cannot wedge the suite.
   */
  stallMs: z.number().int().min(0).max(60_000).optional(),
  /** The effective power mode the mock reports. */
  powerMode: z.enum(["auto_saver", "interactive", "always_on"]).optional(),
  /** Report a desired mode the device has not taken up yet. */
  powerPendingWake: z.boolean().optional(),
  /**
   * Forget the device's pairing, so every authenticated call is refused with
   * `not_provisioned`.
   *
   * The one way a browser test can reproduce a device that answers and says
   * no *permanently* — a panel that was re-paired with something else, or
   * factory reset. That case has to be told apart from a sleeping one: a
   * sleeping device holds a queued push, a refusing device must fail it.
   */
  unpair: z.boolean().optional(),
  /**
   * Run one background device pass, the same one the LaunchAgent's scheduler
   * runs on its timer.
   *
   * The browser suite does not start the scheduler — a thirty-second timer
   * inside a test run is a flake generator — so without this the automatic
   * delivery of a queued frame could only ever be tested by clicking the
   * manual button, which is a different code path from the one that matters
   * in the field. This runs the real function, once, synchronously.
   */
  runDeviceTick: z.boolean().optional(),
});

export const POST = guarded(
  { mutating: true, schema: BodySchema, action: "test.mockDevice" },
  async ({ body }) => {
    if (process.env.NOTE4C_TOWER_E2E !== "1") {
      return jsonError(404, "not_found", "No such route");
    }

    const { device } = await getMockDevice();
    if (body.reset) device.reset();
    if (body.failAck !== undefined) device.failAck = body.failAck;
    if (body.panelDelayMs !== undefined) device.panelDelayMs = body.panelDelayMs;
    if (body.asleep !== undefined) device.asleep = body.asleep;
    if (body.stallMs !== undefined) device.stallMs = body.stallMs;
    if (body.powerMode !== undefined) {
      device.config.power.mode = body.powerMode;
      device.powerWindowEndsAtMs =
        body.powerMode === "interactive" ? Date.now() + 15 * 60_000 : null;
    }
    if (body.powerPendingWake !== undefined) {
      device.powerAck = body.powerPendingWake ? "pending_wake" : null;
      device.powerDesiredMode = body.powerPendingWake ? "always_on" : null;
    }
    if (body.unpair !== undefined) device.token = body.unpair ? null : mockToken();
    if (body.bumpRevision) {
      device.bumpRevisionLocally((config) => {
        if (body.slideMin !== undefined) config.gallery.slide_min = body.slideMin;
      });
    }

    // The whole purpose of this route is to change the device under the tower.
    // The Overview snapshot is a cached reading of how the device was before
    // that, so it has to go, or a spec that puts the mock to sleep would spend
    // the next five minutes being shown a mock that was awake.
    invalidateOverviewSnapshot();

    // After the knobs, so a spec can put the device back on the network and
    // run the pass that notices in one request.
    const deviceTick = body.runDeviceTick ? await runDeviceWindowTick() : null;
    if (deviceTick) invalidateOverviewSnapshot();

    return NextResponse.json({
      deviceTick,
      /**
       * Frame writes the mock actually served, since the last reset.
       *
       * Status 0 is the mock destroying the socket while asleep — an attempt
       * that reached nothing. Counting those would make an "exactly once"
       * assertion a statement about the tower's optimism rather than about
       * what the panel received.
       */
      framePuts: device.requestLog.filter(
        (entry) =>
          entry.method === "PUT" &&
          entry.path.endsWith("/frame") &&
          entry.status !== 0,
      ).length,
      asleep: device.asleep,
      stallMs: device.stallMs,
      failAck: device.failAck,
      panelDelayMs: device.panelDelayMs,
      configRevision: device.configRevision,
      performedActions: device.performedActions,
      snapshot: device.snapshot(),
    });
  },
);
