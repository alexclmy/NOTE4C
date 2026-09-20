/**
 * SSRF-safe device addressing.
 *
 * The tower stores exactly one outbound destination and it is a device on the
 * home LAN. Accepting a hostname would put DNS between the check and the
 * request, so hostnames are refused outright and the whole DNS rebinding class
 * disappears. The tower must never proxy an arbitrary URL.
 */

export class DeviceAddressError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "DeviceAddressError";
  }
}

export type DeviceMode = "mock" | "real";

/**
 * The complete set of paths the tower will ever request.
 *
 * A closed list, not a prefix rule. The device serves legacy unauthenticated
 * gallery routes on the same port, and a prefix check is one typo away from
 * letting the tower call one of them.
 */
export const ALLOWED_PATHS = [
  "/api/v1/dashboard/frame",
  "/api/v1/dashboard/status",
  "/api/v1/dashboard/refresh",
  "/api/v1/dashboard/pair",
  "/api/v1/voice/hub",
  // api 2. Absent from an api 1 device, which answers 404; the tower gates on
  // the negotiated capability list rather than finding out that way.
  "/api/v1/config",
  "/api/v1/actions/restart",
  "/api/v1/actions/sleep",
] as const;
export type AllowedPath = (typeof ALLOWED_PATHS)[number];

export function isIpv4Literal(value: string): boolean {
  const parts = value.split(".");
  if (parts.length !== 4) return false;
  return parts.every((part) => {
    if (!/^\d{1,3}$/.test(part)) return false;
    const n = Number(part);
    // Reject leading zeros: some resolvers read "060" as octal 48, so an
    // address that passes this check must mean exactly one thing.
    return n >= 0 && n <= 255 && String(n) === part;
  });
}

/** RFC1918 private space only: 10/8, 172.16/12, 192.168/16. */
export function isRfc1918(value: string): boolean {
  if (!isIpv4Literal(value)) return false;
  const [a, b] = value.split(".").map(Number) as [number, number, number, number];
  if (a === 10) return true;
  if (a === 172 && b >= 16 && b <= 31) return true;
  if (a === 192 && b === 168) return true;
  return false;
}

export function isLoopback(value: string): boolean {
  return isIpv4Literal(value) && value.split(".")[0] === "127";
}

export interface ResolvedEndpoint {
  origin: string;
  host: string;
  port: number;
}

/**
 * Turn stored configuration into an origin the client may talk to.
 *
 * Real mode: an RFC1918 IPv4 literal on port 80, which is where the device's
 * existing httpd lives. Mock mode: loopback only, on whatever ephemeral port
 * the in-repo mock bound.
 */
export function resolveEndpoint(
  mode: DeviceMode,
  address: string,
  mockOrigin?: string | null,
): ResolvedEndpoint {
  if (mode === "mock") {
    if (!mockOrigin) {
      throw new DeviceAddressError(
        "Mock mode is selected but no mock device is running",
      );
    }
    let url: URL;
    try {
      url = new URL(mockOrigin);
    } catch {
      throw new DeviceAddressError("Mock device origin is not a valid URL");
    }
    if (url.protocol !== "http:" || !isLoopback(url.hostname)) {
      throw new DeviceAddressError(
        "A mock device may only be reached over http on 127.0.0.0/8",
      );
    }
    const port = Number(url.port || "80");
    return { origin: `http://${url.hostname}:${port}`, host: url.hostname, port };
  }

  const trimmed = address.trim();
  if (!isIpv4Literal(trimmed)) {
    throw new DeviceAddressError(
      "The device address must be a plain IPv4 address, not a hostname",
    );
  }
  if (!isRfc1918(trimmed)) {
    throw new DeviceAddressError(
      "The device address must be in private RFC1918 space (10/8, 172.16/12 or 192.168/16)",
    );
  }
  // Port is fixed: the device serves the API on the httpd it already runs.
  return { origin: `http://${trimmed}:80`, host: trimmed, port: 80 };
}

export function assertAllowedPath(path: string): AllowedPath {
  const found = ALLOWED_PATHS.find((allowed) => allowed === path);
  if (!found) {
    throw new DeviceAddressError(`Path "${path}" is not in the allowed set`);
  }
  return found;
}
