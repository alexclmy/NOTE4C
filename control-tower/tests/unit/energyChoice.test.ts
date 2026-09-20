import { describe, expect, it } from "vitest";
import { DevicePowerSchema, energyChoice, type DevicePower } from "@/core/power";

/**
 * Which of the three energy cards is selected.
 *
 * The Energy card offers three choices and the firmware has two modes plus a
 * number, so this is the translation between them. The rule it has to keep:
 * **the selection reflects what the device reports, never what was asked for.**
 * A request the device has not picked up is shown as pending in its own banner;
 * selecting its card as well would be the interface agreeing with itself rather
 * than with the hardware.
 */
function power(patch: Partial<DevicePower>): DevicePower {
  return DevicePowerSchema.parse({
    mode: "auto_saver",
    desired_mode: "auto_saver",
    ack: "acknowledged",
    awake: true,
    sleep_intent: false,
    interactive_remaining_s: 0,
    wake_interval_min: 60,
    timer_armed: true,
    next_wake_in_s: 600,
    next_wake_epoch: null,
    last_wake_reason: "timer",
    last_outcome: "updated",
    consecutive_failures: 0,
    battery: { present: true, calibrated: true, plausible: true, mv: 3900, percent: 57 },
    charge: { state: "not_charging", charging: false },
    ...patch,
  });
}

describe("the energy choice", () => {
  it("selects nothing when the device has not been read", () => {
    // Not "Balanced by default": the tower does not know, and a selected card
    // would be a claim about a device it has not reached.
    expect(energyChoice(null)).toBeNull();
  });

  it("calls always_on Always ready", () => {
    expect(energyChoice(power({ mode: "always_on" }))).toBe("ready");
  });

  it("splits auto_saver by its wake interval", () => {
    expect(energyChoice(power({ wake_interval_min: 15 }))).toBe("balanced");
    expect(energyChoice(power({ wake_interval_min: 60 }))).toBe("balanced");
    expect(energyChoice(power({ wake_interval_min: 240 }))).toBe("balanced");
    // Twice a day and anything sleepier is Deep saver.
    expect(energyChoice(power({ wake_interval_min: 720 }))).toBe("saver");
    expect(energyChoice(power({ wake_interval_min: 1440 }))).toBe("saver");
  });

  /**
   * An open interactive window is none of the three.
   *
   * It is temporary by construction — the device returns to power saving on its
   * own when it runs out — so highlighting a card would say the device had been
   * *set* to something it is going to stop being in a few minutes.
   */
  it("selects no card while an interactive window is open", () => {
    expect(energyChoice(power({ mode: "interactive", interactive_remaining_s: 600 }))).toBeNull();
  });

  it("follows the effective mode, not the requested one", () => {
    const requested = power({
      mode: "auto_saver",
      desired_mode: "always_on",
      ack: "pending_wake",
    });
    // The device says it is still in power saving, so that is what is selected.
    expect(energyChoice(requested)).toBe("balanced");
  });
});
