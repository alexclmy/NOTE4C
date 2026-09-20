import { MockDevice } from "@mock/server";

/**
 * The mock device the tower talks to whenever deviceMode is "mock".
 *
 * It runs in this process rather than as a child. The mock is a plain Node
 * HTTP server with no native dependency and no shared state, so a child
 * process would buy no isolation while adding a lifecycle to get wrong; the
 * same reasoning the plan used to keep the push pipeline in process. The
 * standalone entry point (mock-device/main.ts) still exists for running one
 * beside the dev server by hand.
 */

declare global {
  // eslint-disable-next-line no-var
  var __note4cMockDevice: MockDevice | undefined;
  // eslint-disable-next-line no-var
  var __note4cMockOrigin: string | undefined;
}

/** Fixed so a dev session survives a hot reload without re-pairing. */
const DEV_TOKEN = "0".repeat(64);

export async function getMockDevice(): Promise<{
  device: MockDevice;
  origin: string;
}> {
  if (globalThis.__note4cMockDevice && globalThis.__note4cMockOrigin) {
    return {
      device: globalThis.__note4cMockDevice,
      origin: globalThis.__note4cMockOrigin,
    };
  }

  const device = new MockDevice({
    token: DEV_TOKEN,
    // Fast enough that the UI transitions are watchable without waiting out a
    // real 26.5 second panel cycle on every click.
    panelDelayMs: Number(process.env.NOTE4C_MOCK_PANEL_MS ?? 2_000),
  });
  const origin = await device.listen(0);

  globalThis.__note4cMockDevice = device;
  globalThis.__note4cMockOrigin = origin;
  return { device, origin };
}

export function mockToken(): string {
  return DEV_TOKEN;
}
