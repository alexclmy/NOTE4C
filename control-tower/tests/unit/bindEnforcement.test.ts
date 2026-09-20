import { afterEach, beforeEach, describe, expect, it } from "vitest";
import {
  DEFAULT_BIND_HOST,
  DEFAULT_BIND_PORT,
  bindAllowed,
  hostFromArgv,
  isLoopbackHost,
  isWildcardHost,
  normaliseHost,
  portFromArgv,
  resolveRequestedBind,
} from "@/server/auth/bindGuard";
import {
  LAUNCHER_HOST_ENV,
  enforceBind,
  observeBind,
} from "@/server/auth/bindEnforcement";
import {
  checkProcessBind,
  lastBindEnforcement,
  resetBindEnforcementForTests,
  watchProcessBind,
} from "@/server/auth/bindStartup";
import { setPassphrase } from "@/server/auth/passphrase";
import { useTempDataRoot } from "./helpers/tempRoot";

/**
 * The half of the bind guard that used to be missing.
 *
 * `tests/unit/auth.test.ts` already proved the policy function returns the
 * right verdicts. Every test in this file is about the part that was not
 * there: resolving a host from the ways an operator can actually ask for one,
 * observing what a running process bound, and turning both into a refusal.
 * Each one fails against the code as it was, because none of this existed.
 */

describe("host parsing", () => {
  it("reads both spellings Next accepts, in both forms", () => {
    expect(hostFromArgv(["--hostname", "0.0.0.0"])).toBe("0.0.0.0");
    expect(hostFromArgv(["--hostname=0.0.0.0"])).toBe("0.0.0.0");
    expect(hostFromArgv(["-H", "192.168.1.5"])).toBe("192.168.1.5");
    expect(hostFromArgv(["-H=192.168.1.5"])).toBe("192.168.1.5");
  });

  it("returns null when no host is named, rather than guessing loopback", () => {
    expect(hostFromArgv([])).toBeNull();
    expect(hostFromArgv(["--port", "8654"])).toBeNull();
  });

  it("takes the last host, which is the one Next would use", () => {
    expect(hostFromArgv(["-H", "127.0.0.1", "--hostname", "0.0.0.0"])).toBe(
      "0.0.0.0",
    );
  });

  it("does not read the next flag as a value", () => {
    expect(hostFromArgv(["--hostname", "--port"])).toBeNull();
  });

  it("reads the port the same way", () => {
    expect(portFromArgv(["--port", "8654"])).toBe("8654");
    expect(portFromArgv(["-p=9000"])).toBe("9000");
    expect(portFromArgv([])).toBeNull();
  });
});

describe("host normalisation", () => {
  it("unwraps the bracketed IPv6 spelling", () => {
    expect(normaliseHost("[::]")).toBe("::");
    expect(normaliseHost("[::1]")).toBe("::1");
  });

  it("treats every spelling of loopback as loopback", () => {
    for (const host of [
      "127.0.0.1",
      "127.0.0.53",
      "::1",
      "[::1]",
      "0:0:0:0:0:0:0:1",
      "::ffff:127.0.0.1",
      "LOCALHOST",
    ]) {
      expect(isLoopbackHost(host), host).toBe(true);
    }
  });

  it("treats every spelling of the wildcard as the wildcard", () => {
    for (const host of ["0.0.0.0", "::", "[::]", "*", "", "0:0:0:0:0:0:0:0"]) {
      expect(isWildcardHost(host), host).toBe(true);
    }
    expect(isWildcardHost("192.168.1.5")).toBe(false);
    expect(isWildcardHost("127.0.0.1")).toBe(false);
  });

  it("refuses the bracketed wildcard, which the old guard let through", () => {
    // `bindAllowed` compared against the literal strings "0.0.0.0" and "::",
    // so `--hostname [::]` — which is how an IPv6 host is written on a command
    // line — passed the wildcard check and bound every interface.
    const verdict = bindAllowed({
      hostname: "[::]",
      passphraseSet: true,
      allowLanFlag: "1",
    });
    expect(verdict.allowed).toBe(false);
    expect(verdict.reason).toMatch(/every interface/);
  });

  it("refuses an empty host, which also means every interface", () => {
    expect(
      bindAllowed({ hostname: "", passphraseSet: true, allowLanFlag: "1" }).allowed,
    ).toBe(false);
  });
});

describe("resolving what the launcher was asked to bind", () => {
  it("defaults to loopback when nothing says otherwise", () => {
    const resolved = resolveRequestedBind({ argv: [], env: {} });
    expect(resolved).toEqual({
      host: DEFAULT_BIND_HOST,
      port: DEFAULT_BIND_PORT,
      from: "default",
    });
  });

  it("takes NOTE4C_TOWER_HOST over the default", () => {
    expect(
      resolveRequestedBind({ argv: [], env: { NOTE4C_TOWER_HOST: "192.168.1.5" } }),
    ).toMatchObject({ host: "192.168.1.5", from: "env" });
  });

  it("takes an argument over the environment", () => {
    expect(
      resolveRequestedBind({
        argv: ["--hostname", "0.0.0.0"],
        env: { NOTE4C_TOWER_HOST: "192.168.1.5" },
      }),
    ).toMatchObject({ host: "0.0.0.0", from: "argv" });
  });
});

/** A listening server, as `process._getActiveHandles()` reports one. */
function serverHandle(address: string, port = 8654): unknown {
  return { address: () => ({ address, family: "IPv4", port }) };
}

/** A connected client socket, which answers the same shape and is not a bind. */
function clientHandle(address: string): unknown {
  return {
    address: () => ({ address, family: "IPv4", port: 51234 }),
    remoteAddress: "192.168.1.9",
  };
}

describe("observing what this process bound", () => {
  it("prefers a real listening socket over every other source", () => {
    const observed = observeBind({
      handles: [serverHandle("0.0.0.0")],
      argv: ["--hostname", "127.0.0.1"],
      env: { [LAUNCHER_HOST_ENV]: "127.0.0.1" },
    });
    expect(observed).toEqual({ hosts: ["0.0.0.0"], source: "listening-socket" });
  });

  it("ignores connected sockets, which are not binds", () => {
    expect(observeBind({ handles: [clientHandle("192.168.1.4")] })).toEqual({
      hosts: [],
      source: "none",
    });
  });

  it("ignores handles that have no address at all", () => {
    expect(
      observeBind({ handles: [{}, { address: 3 }, { address: () => null }] }),
    ).toEqual({ hosts: [], source: "none" });
  });

  it("survives a handle that throws while closing", () => {
    const throwing = {
      address: () => {
        throw new Error("closing");
      },
    };
    expect(
      observeBind({ handles: [throwing, serverHandle("127.0.0.1")] }),
    ).toEqual({ hosts: ["127.0.0.1"], source: "listening-socket" });
  });

  it("reports every distinct host a dual-stack listener holds", () => {
    const observed = observeBind({
      handles: [serverHandle("127.0.0.1"), serverHandle("0.0.0.0", 8655)],
    });
    expect(observed.hosts).toEqual(["127.0.0.1", "0.0.0.0"]);
  });

  it("falls back to the launcher's own record, then argv, then HOSTNAME", () => {
    expect(
      observeBind({ env: { [LAUNCHER_HOST_ENV]: "127.0.0.1", HOSTNAME: "0.0.0.0" } }),
    ).toEqual({ hosts: ["127.0.0.1"], source: "launcher" });
    expect(
      observeBind({ argv: ["-H", "192.168.1.5"], env: { HOSTNAME: "0.0.0.0" } }),
    ).toEqual({ hosts: ["192.168.1.5"], source: "argv" });
    expect(observeBind({ env: { HOSTNAME: "0.0.0.0" } })).toEqual({
      hosts: ["0.0.0.0"],
      source: "env",
    });
  });

  it("says none rather than inventing loopback", () => {
    expect(observeBind({})).toEqual({ hosts: [], source: "none" });
  });
});

describe("enforcing the policy on an observation", () => {
  const noPassphrase = { passphraseSet: false, allowLanFlag: undefined };
  const everythingSet = { passphraseSet: true, allowLanFlag: "1" };

  it("refuses 0.0.0.0 with no passphrase and no flag", () => {
    const result = enforceBind(
      { hosts: ["0.0.0.0"], source: "listening-socket" },
      noPassphrase,
    );
    expect(result.verdict).toBe("refused");
    expect(result.refusals).toHaveLength(1);
    expect(result.refusals[0]).toMatch(/NOTE4C_TOWER_ALLOW_LAN=1/);
  });

  it("refuses a non-loopback host with no passphrase, even with the flag", () => {
    const result = enforceBind(
      { hosts: ["192.168.1.5"], source: "listening-socket" },
      { passphraseSet: false, allowLanFlag: "1" },
    );
    expect(result.verdict).toBe("refused");
    expect(result.refusals[0]).toMatch(/passphrase/);
  });

  it("refuses the wildcard even when both conditions are met", () => {
    for (const host of ["0.0.0.0", "::", "[::]"]) {
      const result = enforceBind(
        { hosts: [host], source: "listening-socket" },
        everythingSet,
      );
      expect(result.verdict, host).toBe("refused");
      expect(result.refusals[0]).toMatch(/every interface/);
    }
  });

  it("allows loopback with nothing configured", () => {
    const result = enforceBind(
      { hosts: ["127.0.0.1"], source: "listening-socket" },
      noPassphrase,
    );
    expect(result.verdict).toBe("allowed");
    expect(result.summary).toMatch(/loopback/);
  });

  it("allows a named LAN address when both conditions are met", () => {
    const result = enforceBind(
      { hosts: ["192.168.1.5"], source: "listening-socket" },
      everythingSet,
    );
    expect(result.verdict).toBe("allowed");
    expect(result.summary).toMatch(/NOTE4C_TOWER_ALLOW_LAN=1/);
  });

  it("refuses when one host of a pair is refused, not just when all are", () => {
    const result = enforceBind(
      { hosts: ["127.0.0.1", "0.0.0.0"], source: "listening-socket" },
      noPassphrase,
    );
    expect(result.verdict).toBe("refused");
    expect(result.refusals).toHaveLength(1);
  });

  it("says undetermined, not allowed, when nothing was observed", () => {
    const result = enforceBind({ hosts: [], source: "none" }, noPassphrase);
    expect(result.verdict).toBe("undetermined");
    expect(result.summary).toMatch(/could not determine/);
    // The distinction the Diagnostics row exists to carry.
    expect(result.verdict).not.toBe("allowed");
  });
});

describe("refusing to serve a process that is already listening", () => {
  let temp: ReturnType<typeof useTempDataRoot>;

  beforeEach(() => {
    temp = useTempDataRoot();
    resetBindEnforcementForTests();
  });

  afterEach(() => {
    temp.dispose();
    resetBindEnforcementForTests();
  });

  function watch(overrides: Parameters<typeof watchProcessBind>[0]) {
    const refused: string[] = [];
    const pending: Array<() => void> = [];
    const logged: string[] = [];
    const first = watchProcessBind({
      ...overrides,
      onRefused: (enforcement) => refused.push(enforcement.summary),
      schedule: (fn) => pending.push(fn),
      log: (message) => logged.push(message),
    });
    return { first, refused, pending, logged };
  }

  it("stops a wildcard bind it can see, with nothing configured", () => {
    const { first, refused, logged } = watch({
      handles: [serverHandle("0.0.0.0")],
      env: {},
    });
    expect(first.verdict).toBe("refused");
    expect(refused).toHaveLength(1);
    expect(logged.join("\n")).toMatch(/refusing to serve/i);
  });

  it("stops a wildcard bind even when a passphrase is set and the LAN is allowed", () => {
    setPassphrase("a sufficiently long tower passphrase");
    const { first, refused } = watch({
      handles: [serverHandle("0.0.0.0")],
      env: { NOTE4C_TOWER_ALLOW_LAN: "1" },
    });
    expect(first.verdict).toBe("refused");
    expect(first.refusals[0]).toMatch(/every interface/);
    expect(refused).toHaveLength(1);
  });

  it("stops a LAN bind when no passphrase is set", () => {
    const { first, refused } = watch({
      handles: [serverHandle("192.168.1.5")],
      env: { NOTE4C_TOWER_ALLOW_LAN: "1" },
    });
    expect(first.verdict).toBe("refused");
    expect(first.refusals[0]).toMatch(/passphrase/);
    expect(refused).toHaveLength(1);
  });

  it("lets a named LAN bind through once both conditions are met", () => {
    setPassphrase("a sufficiently long tower passphrase");
    const { first, refused } = watch({
      handles: [serverHandle("192.168.1.5")],
      env: { NOTE4C_TOWER_ALLOW_LAN: "1" },
    });
    expect(first.verdict).toBe("allowed");
    expect(refused).toHaveLength(0);
  });

  it("says nothing and refuses nothing about a loopback bind", () => {
    const { first, refused, logged } = watch({
      handles: [serverHandle("127.0.0.1")],
      env: {},
    });
    expect(first.verdict).toBe("allowed");
    expect(refused).toHaveLength(0);
    expect(logged).toHaveLength(0);
  });

  it("re-checks after start-up, because the socket may not exist yet", () => {
    // The case this exists for: instrumentation runs before Next binds, so the
    // first pass sees nothing. A guard that only looked once would report
    // "undetermined" and never notice the wildcard that arrived a second later.
    // Mutated in place rather than reassigned: the watcher holds the array,
    // which is exactly how the real `process._getActiveHandles()` result grows
    // as Next binds.
    const handles: unknown[] = [];
    const refused: string[] = [];
    const pending: Array<() => void> = [];
    const first = watchProcessBind({
      handles,
      env: {},
      onRefused: (enforcement) => refused.push(enforcement.summary),
      schedule: (fn) => pending.push(fn),
      log: () => {},
    });

    expect(first.verdict).toBe("undetermined");
    expect(refused).toHaveLength(0);
    expect(pending.length).toBeGreaterThan(0);

    handles.push(serverHandle("0.0.0.0"));
    for (const run of pending) run();
    expect(refused.length).toBeGreaterThan(0);
  });

  it("does not let a later blind pass overwrite a verdict it already reached", () => {
    watch({ handles: [serverHandle("127.0.0.1")], env: {} });
    expect(lastBindEnforcement()?.verdict).toBe("allowed");
    checkProcessBind({ handles: [], argv: [], env: {} });
    expect(lastBindEnforcement()?.verdict).toBe("allowed");
  });
});
