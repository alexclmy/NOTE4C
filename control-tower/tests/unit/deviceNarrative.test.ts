import { describe, expect, it } from "vitest";
import {
  DEVICE_ACTION_COPY,
  DEVICE_STATES,
  NO_REMOTE_WAKE_NOTE,
  describeWhatHappensNext,
} from "@/core/power";

/**
 * The narrative that sits under the device's state word.
 *
 * "What we know" is `deriveDeviceState().reason`, which the state suite already
 * covers. These are the other two answers — what can be done now, and what
 * happens next — and the properties worth pinning are not the wording but the
 * promises: that every state has both answers, and that neither of them ever
 * offers to wake a device that cannot be woken.
 */
describe("the device narrative", () => {
  it("answers both questions for every state, with no gaps", () => {
    for (const state of DEVICE_STATES) {
      const canDo = DEVICE_ACTION_COPY[state];
      expect(canDo, `${state} has no "what you can do"`).toBeTruthy();
      expect(canDo.length).toBeGreaterThan(20);

      const next = describeWhatHappensNext(state, "07:30");
      expect(next, `${state} has no "what happens next"`).toBeTruthy();
      expect(next.length).toBeGreaterThan(20);
    }
  });

  /**
   * The one sentence this product may never write.
   *
   * A sleeping device has its radio powered down. Any copy that offers to wake
   * it is a lie the interface tells about the hardware, and this is the table
   * most likely to grow one, because "what you can do now" is exactly the slot
   * a well-meaning edit would put it in.
   */
  it("never offers to wake the device over the network", () => {
    for (const state of DEVICE_STATES) {
      const both = `${DEVICE_ACTION_COPY[state]} ${describeWhatHappensNext(state, "07:30")}`;
      expect(both.toLowerCase()).not.toMatch(/\bwake it up\b|\bwake the device\b/);
    }
    // And the canonical note still says why, so the UI has something to point
    // at instead of inventing its own explanation.
    expect(NO_REMOTE_WAKE_NOTE).toMatch(/no command from here can wake it/);
  });

  /**
   * A template with an empty slot in it is worse than a shorter sentence.
   *
   * The tower has no next wake to name until it has seen the device at least
   * once, and "Next wake around ." is the sort of thing that ships because the
   * fixture always had a value.
   */
  it("names a wake time only when there is one", () => {
    const withWake = describeWhatHappensNext("asleep", "07:30");
    expect(withWake).toContain("07:30");

    const without = describeWhatHappensNext("asleep", null);
    expect(without).not.toContain("around .");
    expect(without).not.toContain("null");
    expect(without).toMatch(/its own schedule/);
  });

  it("tells an asleep device from an unreachable one", () => {
    // Asleep is the product working, so its "can do" is about preparing work.
    expect(DEVICE_ACTION_COPY.asleep).toMatch(/queue/i);
    // Unreachable is a fault, so its "can do" is about the physical device.
    expect(DEVICE_ACTION_COPY.unreachable).toMatch(/power and Wi-Fi/);
    // And neither of them claims anything was lost: e-paper holds its image.
    expect(DEVICE_ACTION_COPY.unreachable).toMatch(/nothing is lost/i);
  });
});
