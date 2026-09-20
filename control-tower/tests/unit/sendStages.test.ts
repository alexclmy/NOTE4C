import { describe, expect, it } from "vitest";
import { BLOCKING_STATES, TERMINAL_STATES, type PushState } from "@/server/device/ledger";
import { stageIndexFor } from "@/ui/sendStages";

/**
 * How far the send dialog says a push has got.
 *
 * This is the function that turns a ledger state into a row of dots, and the
 * property it has to keep is the one the whole dialog is built on: **a stage is
 * lit because a line was written, never because time passed.** So the mapping
 * is total over the ledger's own vocabulary, it never runs ahead of what the
 * pipeline has recorded, and the final stage belongs to exactly one state.
 */
describe("the send flow's stage mapping", () => {
  it("lights nothing before the pipeline has written anything", () => {
    expect(stageIndexFor(null)).toBe(0);
  });

  it("advances only on states the pipeline actually appends", () => {
    // Accepted, nothing on the wire yet: the first stage is done.
    expect(stageIndexFor("pending")).toBe(1);
    // The device acknowledged the frame and returned a sequence number, so
    // "sent" and "stored" are both facts and the panel is the open question.
    expect(stageIndexFor("sent")).toBe(3);
    // Every stage, and only for the one state that means the device confirmed.
    expect(stageIndexFor("verified_displayed")).toBe(5);
  });

  /**
   * The states that are not progress.
   *
   * `uncertain`, `failed`, `acknowledged` and `queued` all end the dialog on a
   * screen of their own rather than on the stage list, so none of them may
   * advance the dots — least of all `uncertain`, where advancing to the last
   * stage would be the interface claiming a confirmation that never arrived.
   */
  it("does not advance for an outcome that is not progress", () => {
    for (const state of ["uncertain", "failed", "acknowledged", "queued"] as const) {
      expect(stageIndexFor(state), `${state} moved the dots`).toBe(0);
    }
  });

  it("is total over the ledger's vocabulary, with no gaps and no throws", () => {
    const every: PushState[] = [
      ...new Set<PushState>([...BLOCKING_STATES, ...TERMINAL_STATES]),
    ];
    // The union of blocking and terminal is every state the ledger defines.
    expect(every.length).toBe(7);
    for (const state of every) {
      const index = stageIndexFor(state);
      expect(Number.isInteger(index)).toBe(true);
      expect(index).toBeGreaterThanOrEqual(0);
      expect(index).toBeLessThanOrEqual(5);
    }
  });

  it("treats a state it has never heard of as no progress at all", () => {
    // Forward compatibility that fails safe: a ledger state added later shows
    // an unfinished send rather than a finished one.
    expect(stageIndexFor("something_new")).toBe(0);
  });
});
