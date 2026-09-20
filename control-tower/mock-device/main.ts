import { MockDevice } from "./server";

/**
 * Standalone entry point, for running the mock beside a dev server:
 *
 *   npm run mock-device -- --port 8655 --panel-delay 3000
 *
 * The tower can also start a mock in process (see src/server/device/mock.ts),
 * which is what `npm run dev` and the Playwright suite use.
 */

function flag(name: string): string | undefined {
  const index = process.argv.indexOf(`--${name}`);
  if (index === -1) return undefined;
  return process.argv[index + 1];
}

async function main(): Promise<void> {
  const device = new MockDevice({
    panelDelayMs: Number(flag("panel-delay") ?? 3000),
    token: flag("token") ?? undefined,
    lockdown: flag("lockdown") !== "off",
    failAck: flag("fail-ack") === "on",
  });

  const origin = await device.listen(Number(flag("port") ?? 0));
  process.stdout.write(`mock device listening on ${origin}\n`);
  // The token is printed once, deliberately, because a standalone mock is
  // useless without it and it is a throwaway value for a simulated device.
  process.stdout.write(`mock device token: ${device.token}\n`);

  const shutdown = (): void => {
    void device.close().then(() => process.exit(0));
  };
  process.on("SIGINT", shutdown);
  process.on("SIGTERM", shutdown);
}

void main();
