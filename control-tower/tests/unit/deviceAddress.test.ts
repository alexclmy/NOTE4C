/**
 * Address validation.
 *
 * The fixtures are RFC1918 addresses because that is exactly what this
 * validator requires — a TEST-NET-1 address would be *refused* by the code
 * under test, so it cannot stand in here. They are deliberately not any
 * particular device's: 192.168.7.7 is a shape, not a panel.
 */
import { describe, expect, it } from "vitest";
import {
  ALLOWED_PATHS,
  DeviceAddressError,
  assertAllowedPath,
  isIpv4Literal,
  isLoopback,
  isRfc1918,
  resolveEndpoint,
} from "@/server/device/address";

describe("IPv4 literal parsing", () => {
  it("accepts well-formed addresses", () => {
    for (const value of ["0.0.0.0", "192.168.7.7", "255.255.255.255", "10.0.0.1"]) {
      expect(isIpv4Literal(value), value).toBe(true);
    }
  });

  it("refuses anything that is not four plain octets", () => {
    for (const value of [
      "192.168.0",
      "192.168.7.7.1",
      "192.168.0.256",
      "192.168.0.-1",
      "192.168.0.060",
      "192.168.0.0x3c",
      "note4c.local",
      "192.168.7.7:80",
      "::1",
      "",
      " 192.168.7.7",
    ]) {
      expect(isIpv4Literal(value), value).toBe(false);
    }
  });
});

describe("RFC1918 classification", () => {
  it("accepts private space", () => {
    for (const value of [
      "10.0.0.1",
      "10.255.255.254",
      "172.16.0.1",
      "172.31.255.254",
      "192.168.7.7",
    ]) {
      expect(isRfc1918(value), value).toBe(true);
    }
  });

  it("refuses public and adjacent ranges", () => {
    for (const value of [
      "8.8.8.8",
      "172.15.0.1",
      "172.32.0.1",
      "192.169.0.1",
      "169.254.169.254",
      "127.0.0.1",
      "0.0.0.0",
    ]) {
      expect(isRfc1918(value), value).toBe(false);
    }
  });

  it("identifies loopback separately", () => {
    expect(isLoopback("127.0.0.1")).toBe(true);
    expect(isLoopback("127.5.4.3")).toBe(true);
    expect(isLoopback("192.168.7.7")).toBe(false);
  });
});

describe("resolveEndpoint in real mode", () => {
  it("pins the port to 80", () => {
    expect(resolveEndpoint("real", "192.168.7.7")).toEqual({
      origin: "http://192.168.7.7:80",
      host: "192.168.7.7",
      port: 80,
    });
  });

  it("refuses a hostname, which removes DNS rebinding entirely", () => {
    expect(() => resolveEndpoint("real", "note4c.local")).toThrow(
      /not a hostname/,
    );
    expect(() => resolveEndpoint("real", "localhost")).toThrow(DeviceAddressError);
  });

  it("refuses a public address", () => {
    expect(() => resolveEndpoint("real", "8.8.8.8")).toThrow(/RFC1918/);
  });

  it("refuses the cloud metadata address", () => {
    expect(() => resolveEndpoint("real", "169.254.169.254")).toThrow(/RFC1918/);
  });

  it("refuses loopback in real mode", () => {
    expect(() => resolveEndpoint("real", "127.0.0.1")).toThrow(/RFC1918/);
  });

  it("tolerates surrounding whitespace in stored configuration", () => {
    expect(resolveEndpoint("real", "  192.168.7.7 ").host).toBe("192.168.7.7");
  });
});

describe("resolveEndpoint in mock mode", () => {
  it("accepts a loopback origin on any port", () => {
    expect(resolveEndpoint("mock", "192.168.7.7", "http://127.0.0.1:54321")).toEqual({
      origin: "http://127.0.0.1:54321",
      host: "127.0.0.1",
      port: 54321,
    });
  });

  it("refuses a non-loopback mock origin", () => {
    expect(() =>
      resolveEndpoint("mock", "192.168.7.7", "http://192.168.7.7:8080"),
    ).toThrow(/127\.0\.0\.0\/8/);
  });

  it("refuses https and other schemes", () => {
    expect(() =>
      resolveEndpoint("mock", "192.168.7.7", "https://127.0.0.1:8080"),
    ).toThrow(DeviceAddressError);
    expect(() =>
      resolveEndpoint("mock", "192.168.7.7", "file:///etc/passwd"),
    ).toThrow(DeviceAddressError);
  });

  it("reports clearly when no mock is running", () => {
    expect(() => resolveEndpoint("mock", "192.168.7.7", null)).toThrow(
      /no mock device is running/,
    );
  });
});

describe("path allowlist", () => {
  it("contains exactly the routes the tower uses, and nothing else", () => {
    expect([...ALLOWED_PATHS]).toEqual([
      "/api/v1/dashboard/frame",
      "/api/v1/dashboard/status",
      "/api/v1/dashboard/refresh",
      "/api/v1/dashboard/pair",
      "/api/v1/voice/hub",
      "/api/v1/config",
      "/api/v1/actions/restart",
      "/api/v1/actions/sleep",
    ]);
  });

  it("has no route that reads a credential out of the device", () => {
    // The hub token goes in through /api/v1/voice/hub and never comes back.
    // Nothing in this list is a route that could return one.
    for (const path of ALLOWED_PATHS) {
      expect(path).not.toMatch(/token|secret|nvs|wifi|ota/i);
    }
  });

  it("refuses the legacy gallery upload path, which grows the gallery", () => {
    expect(() => assertAllowedPath("/upload")).toThrow(DeviceAddressError);
    expect(() => assertAllowedPath("/photo")).toThrow(DeviceAddressError);
  });

  it("refuses traversal and near misses", () => {
    for (const path of [
      "/api/v1/dashboard/frame/../../../etc/passwd",
      "/api/v1/dashboard/Frame",
      "/api/v2/config",
      "",
    ]) {
      expect(() => assertAllowedPath(path), path).toThrow(DeviceAddressError);
    }
  });
});
