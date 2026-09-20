import { DeviceClient } from "./client";
import { getMockDevice, mockToken } from "./mock";
import { readDeviceToken } from "./token";
import { readState, updateState, type TowerState } from "../store/state";

export interface DeviceContext {
  client: DeviceClient;
  state: TowerState;
  simulated: boolean;
  tokenConfigured: boolean;
}

/**
 * Build the client the rest of the server talks through.
 *
 * In mock mode the in-process mock is started on demand and its loopback
 * origin recorded, so nothing about the device path differs between mock and
 * real except the endpoint and the banner.
 */
export async function deviceContext(
  options: { timeoutMs?: number } = {},
): Promise<DeviceContext> {
  let state = readState();

  if (state.deviceMode === "mock") {
    const { origin } = await getMockDevice();
    if (state.mockDeviceOrigin !== origin) {
      state = updateState({ mockDeviceOrigin: origin });
    }
    return {
      client: new DeviceClient({
        mode: "mock",
        address: state.deviceAddress,
        mockOrigin: origin,
        token: mockToken(),
        timeoutMs: options.timeoutMs,
      }),
      state,
      simulated: true,
      tokenConfigured: true,
    };
  }

  const token = readDeviceToken();
  return {
    client: new DeviceClient({
      mode: "real",
      address: state.deviceAddress,
      token,
      // A caller may ask for a shorter status/config timeout — the fast-catch
      // probe does, so a sleeping panel fails in a couple of seconds and the
      // next probe comes quickly. Frame uploads keep their own longer budget.
      timeoutMs: options.timeoutMs,
    }),
    state,
    simulated: false,
    tokenConfigured: token !== null,
  };
}
