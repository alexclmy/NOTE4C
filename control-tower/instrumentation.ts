/**
 * Process start-up: the bind guard, and the one background job this product
 * has.
 *
 * The shape of the scheduler guard matters and is not stylistic. Next compiles
 * this file for every runtime it might run in, including the edge one, and
 * webpack only drops an `import()` when the branch holding it can be folded
 * away at build time. Written as an early `return` for the wrong runtime —
 * which is how this was first written — the dynamic import stays reachable,
 * the edge build tries to bundle the scheduler, and the scheduler reaches
 * `node:child_process` through the Reminders adapter. That fails the whole
 * compilation, and the development server then answers 500 to every request,
 * including the one the browser suite waits on to decide the server is up.
 *
 * So: the runtime check is a positive `if` around the import, which
 * `process.env.NEXT_RUNTIME` makes a compile-time constant, and the edge build
 * never sees the scheduler at all.
 */
export async function register(): Promise<void> {
  if (process.env.NEXT_RUNTIME === "nodejs") {
    // The bind guard, second line. The first is tools/tower-serve.ts, which
    // decides the host before this process exists; this one covers a server
    // somebody started another way, and refuses to go on if it can see that
    // the socket is on a host the policy does not allow. It is deliberately
    // unconditional: a build, a test and a real tower should all refuse a
    // wildcard bind, and refusing costs nothing when the bind is loopback.
    const { watchProcessBind } = await import("@/server/auth/bindStartup");
    watchProcessBind();

    // Explicitly enabled only by the persistent LaunchAgent. Builds, tests and
    // developer servers must never open the real device push pipeline.
    if (process.env.NOTE4C_TOWER_SCHEDULER !== "1") return;
    const { startRefreshScheduler } = await import("@/server/refreshScheduler");
    startRefreshScheduler();
  }
}
