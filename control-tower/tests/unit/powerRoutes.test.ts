/**
 * Where a power intent is allowed to be delivered from, and where it is not.
 *
 * The finding these pin: `GET /api/device/status` called `reconcileIntent`,
 * so reading the status page could PATCH the device's configuration and
 * rewrite the tower's durable state. That is a mutation on a route with no
 * CSRF check, reachable from anything that could cause a browser to issue a
 * GET, and it made "look at the page" and "change the device" the same action.
 *
 * Delivery now has exactly two homes, both of which a user or an operator has
 * asked for: `POST /api/device/power` (session + CSRF, like every other
 * mutation) and the background scheduler. This file checks both ends of that.
 */

import { readFileSync } from "node:fs";
import { resolve } from "node:path";
import { afterEach, beforeEach, describe, expect, it } from "vitest";
import { MockDevice } from "@mock/server";
import { runPowerIntentTick } from "@/server/refreshScheduler";
import { readIntent, recordIntent } from "@/server/device/powerIntent";
import { redactParams } from "@/server/audit";
import { readState, updateState } from "@/server/store/state";
import { useTempDataRoot } from "./helpers/tempRoot";

const ROOT = resolve(__dirname, "../..");

function source(relative: string): string {
  return readFileSync(resolve(ROOT, relative), "utf8");
}

describe("the status route is a read", () => {
  const STATUS_ROUTE = "app/api/device/status/route.ts";

  /**
   * A structural check, deliberately, because there is no route harness here
   * and the property worth protecting is architectural rather than behavioural:
   * this module must not be able to reach a writer. Naming the writers means a
   * future edit that reintroduces one fails here rather than in the field.
   */
  it("imports nothing from powerIntent that writes", () => {
    const text = source(STATUS_ROUTE);
    const writers = [
      "reconcileIntent",
      "recordIntent",
      "clearIntent",
      "noteDeviceSeen",
      "sleepNow",
    ];
    for (const writer of writers) {
      // Allowed in prose, not in code. Strip block comments before looking.
      const code = text.replace(/\/\*[\s\S]*?\*\//g, "").replace(/\/\/.*$/gm, "");
      expect(code, `${STATUS_ROUTE} must not call ${writer}`).not.toContain(writer);
    }
  });

  it("does not write the tower's state file either", () => {
    const code = source(STATUS_ROUTE)
      .replace(/\/\*[\s\S]*?\*\//g, "")
      .replace(/\/\/.*$/gm, "");
    expect(code).not.toContain("updateState");
    // It still reads it, which is the point.
    expect(code).toContain("readState");
  });

  it("uses the non-mutating description instead", () => {
    expect(source(STATUS_ROUTE)).toContain("describeIntent");
  });
});

describe("the power route is where mutations live", () => {
  const POWER_ROUTE = "app/api/device/power/route.ts";

  it("is guarded as mutating, so it gets the CSRF check", () => {
    const text = source(POWER_ROUTE);
    expect(text).toContain("mutating: true");
    expect(text).toContain("reconcileIntent");
  });

  /**
   * The explicit home for what the GET used to do implicitly. A user who has
   * just pressed the button on the device needs a way to say "try it now"
   * rather than waiting for the next scheduler pass.
   */
  it("offers an explicit reconcile action", () => {
    expect(source(POWER_ROUTE)).toContain('z.literal("reconcile")');
  });

  /**
   * Cancelling answers in the same shape as everything else on this route.
   * It used to return only `{intent, cancelled}`, so the client read `detail`
   * as undefined and showed the user nothing at all after they clicked
   * "cancel this request" — indistinguishable from a broken button.
   */
  it("answers a cancel with a detail the UI can show", () => {
    const text = source(POWER_ROUTE);
    const cancel = text.slice(
      text.indexOf('body.action === "cancel-intent"'),
      text.indexOf("const { client, simulated, tokenConfigured }"),
    );

    // Both outcomes exist: there was something to withdraw, or there was not.
    expect(cancel).toContain("cancelled: true");
    expect(cancel).toContain("cancelled: false");

    // And each of the two *responses* carries a sentence. Counting `detail`
    // across the whole block is not enough — the appendAudit call has one too,
    // and the audit log is not what the user reads. So look inside each
    // NextResponse.json literal on its own.
    const responses = [...cancel.matchAll(/NextResponse\.json\(\{[\s\S]*?\}\)/g)].map(
      (m) => m[0],
    );
    expect(responses).toHaveLength(2);
    for (const response of responses) {
      // `detail:` or the shorthand `detail,` — both put a sentence in the body.
      expect(response).toMatch(/\bdetail\s*[,:]/);
      // Same shape as every other action on this route, so one client branch
      // renders them all.
      expect(response).toContain("applied:");
      expect(response).toContain("pending:");
    }
  });
});

describe("the scheduler's power pass", () => {
  const TOKEN = "e".repeat(64);
  let temp: ReturnType<typeof useTempDataRoot>;
  let device: MockDevice;

  beforeEach(async () => {
    temp = useTempDataRoot();
    device = new MockDevice({ token: TOKEN, panelDelayMs: 10 });
    await device.listen(0);
  });

  afterEach(async () => {
    await device.close();
    temp.dispose();
  });

  it("does nothing, and opens no socket, when no intent is recorded", async () => {
    updateState({ deviceMode: "real" });
    expect(await runPowerIntentTick()).toBe("no_intent");
    expect(device.requestLog).toHaveLength(0);
  });

  it("leaves a mock device alone", async () => {
    recordIntent({
      mode: "always_on",
      interactiveMinutes: null,
      wakeIntervalMinutes: null,
    });
    updateState({ deviceMode: "mock" });
    expect(await runPowerIntentTick()).toBe("not_real");
    expect(readIntent()).not.toBeNull();
  });

  /**
   * The unreachable branch is deliberately *not* exercised here.
   *
   * `resolveEndpoint` pins real mode to port 80 on an RFC1918 address, so any
   * test that drives this pass in real mode opens a connection to whatever is
   * at that address on the machine running the suite — which, on a developer's
   * own network, is the actual device. A unit test must not reach out and
   * touch it. The same code path is covered without a network in
   * powerIntent.test.ts, where the mock's `asleep` flag reproduces a device
   * whose radio is off and `reconcileIntent` is driven directly.
   */
});

/**
 * Refusing a request must not destroy an unrelated one.
 *
 * `POST /api/device/power` with `set-mode` used to call `recordIntent` first
 * and check the device's capabilities second, with `clearIntent()` on the
 * refusal path. `clearIntent` does not undo an overwrite — it deletes whatever
 * is in the state file. So a tower holding a pending intent for a sleeping
 * device, asked to set a mode while the device happened to be awake on a build
 * with no hybrid contract, answered 400 and threw the pending request away as
 * a side effect.
 *
 * Checked structurally, in source order, because there is no route harness in
 * this suite and the property is an ordering one: the capability gate has to
 * come before the write, and no branch may reach `clearIntent` on the way to
 * refusing. A behavioural test would prove today's code correct; this proves
 * the shape that made it correct, which is the thing an edit can undo.
 */
describe("the power route checks capability before it overwrites an intent", () => {
  const POWER_ROUTE = "app/api/device/power/route.ts";

  function code(): string {
    return source(POWER_ROUTE)
      .replace(/\/\*[\s\S]*?\*\//g, "")
      .replace(/\/\/.*$/gm, "");
  }

  it("gates on deviceSupportsPower before calling recordIntent", () => {
    const text = code();
    const gate = text.lastIndexOf("deviceSupportsPower");
    const write = text.indexOf("recordIntent(");
    expect(gate, "the route no longer checks the capability at all").toBeGreaterThan(-1);
    expect(write, "the route no longer records an intent").toBeGreaterThan(-1);
    expect(
      gate,
      "recordIntent runs before the capability gate, so a refusal overwrites a pending intent",
    ).toBeLessThan(write);
  });

  it("never clears an intent on the unsupported-firmware path", () => {
    const text = code();
    const unsupported = text.indexOf("does not advertise the hybrid power contract, so the tower did not write");
    expect(unsupported).toBeGreaterThan(-1);
    // The only clearIntent in this file belongs to `cancel-intent`, which is a
    // user explicitly withdrawing a request, and it is far earlier in the file.
    const cancelBlock = text.indexOf('body.action === "cancel-intent"');
    const clears = [...text.matchAll(/clearIntent\(\)/g)].map((m) => m.index ?? -1);
    expect(clears.length, "more than one clearIntent in this route").toBe(1);
    expect(
      clears[0],
      "clearIntent moved out of the cancel-intent branch",
    ).toBeGreaterThan(cancelBlock);
    expect(
      clears[0],
      "clearIntent is on the refusal path again",
    ).toBeLessThan(unsupported);
  });
});

/**
 * The passphrase is never handed to the audit writer.
 *
 * `appendAudit` redacts any parameter whose key looks like a credential, and
 * the first-run route relied on that: it passed `{ passphrase: <plaintext> }`
 * and trusted the pattern to catch it. That is a net under a tightrope nobody
 * needed to walk. The plaintext existed as a live argument inside a function
 * whose job is to serialise its argument, one edit to the redaction pattern
 * away from a file on disk, and visible to anything that captured a stack in
 * between.
 *
 * Both halves are checked: the value never crosses the boundary, AND the
 * redaction still works for anything that ever does.
 */
describe("secrets are not handed to the audit writer at all", () => {
  it("the first-run route passes no params to appendAudit", () => {
    const text = source("app/api/auth/setup/route.ts")
      .replace(/\/\*[\s\S]*?\*\//g, "")
      .replace(/\/\/.*$/gm, "");
    const call = text.slice(text.indexOf("appendAudit({"));
    const entry = call.slice(0, call.indexOf("});") + 3);
    expect(entry).toContain('action: "auth.setup"');
    expect(entry, "the audit entry carries a params object again").not.toContain(
      "params:",
    );
    // The word survives in `detail` — "Tower passphrase set on first run" is
    // exactly the fact worth recording. What must not survive is the VALUE,
    // which can only get here by reading it off the parsed body.
    expect(
      entry,
      "the plaintext passphrase reaches appendAudit again",
    ).not.toContain("parsed.data.passphrase");
    expect(entry).not.toMatch(/passphrase\s*:/);
  });

  it("the voice hub route records that a token was supplied, not the token", () => {
    const text = source("app/api/voice/hub/route.ts")
      .replace(/\/\*[\s\S]*?\*\//g, "")
      .replace(/\/\/.*$/gm, "");
    expect(text).toContain("tokenSupplied");
    expect(
      text,
      "the hub token is passed to appendAudit again",
    ).not.toMatch(/params:\s*\{[^}]*\btoken:\s*body\.token/);
  });

  it("and the redaction is still there, for anything that ever does", () => {
    const redacted = redactParams({
      deviceToken: "secret-value",
      nested: { hubToken: "another-secret" },
      mode: "always_on",
    });
    expect(JSON.stringify(redacted)).not.toContain("secret-value");
    expect(JSON.stringify(redacted)).not.toContain("another-secret");
    expect(redacted.mode).toBe("always_on");
  });
});
