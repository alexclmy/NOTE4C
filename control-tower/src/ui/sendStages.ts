/**
 * The five things that happen when a frame goes to the panel, and how far
 * along the ledger says one is.
 *
 * Kept out of the dialog that draws it for two reasons. It is the piece of the
 * send flow with an actual rule in it, and it is the piece that has to be
 * provable: the promise the whole dialog rests on is that **a stage is lit
 * because a line was written, never because time passed**, and that is a
 * property of this function rather than of any pixel.
 *
 * The wording is the user's, not the wire's — "Frame sent to the device"
 * rather than `PUT /api/v1/dashboard/frame`. The note on the fourth stage is
 * the one piece of copy in the flow that stops a support question: an e-paper
 * panel inverting itself for twenty-five seconds looks exactly like a fault to
 * somebody who has not been told it is how the pigments develop.
 */

export interface SendStage {
  name: string;
  note: string;
}

export const SEND_STAGES: readonly SendStage[] = [
  { name: "Request accepted", note: "" },
  { name: "Frame sent to the device", note: "" },
  { name: "Stored on the device", note: "" },
  {
    name: "Panel refreshing",
    note: "The panel blinks for about 25 seconds. That is how e-paper develops its four pigments — not a fault.",
  },
  { name: "Displayed — confirmed by the device", note: "" },
];

/**
 * How many stages the ledger says are finished.
 *
 * Zero means nothing has been recorded yet; five means every stage is done.
 * The three states that move it are the three the pipeline actually appends
 * while a push is in flight, and the mapping of `sent` to *two* finished
 * stages is not a shortcut: the device's acknowledgement of a frame carries
 * the sequence number it stored it under, so "sent" and "stored" are one fact.
 *
 * Everything else returns zero, and that includes the outcomes. `uncertain`,
 * `failed`, `acknowledged` and `queued` each end the flow on a screen of their
 * own; advancing the dots for `uncertain` in particular would be the interface
 * claiming a confirmation that never arrived. An unrecognised state returns
 * zero for the same reason — a state added to the ledger later should show an
 * unfinished send rather than a finished one.
 */
export function stageIndexFor(state: string | null): number {
  switch (state) {
    case "pending":
      // Written the moment the push is accepted, before anything is on the
      // wire. Stage 0 is done; the tower is now working on stage 1.
      return 1;
    case "sent":
      return 3;
    case "verified_displayed":
      return SEND_STAGES.length;
    default:
      return 0;
  }
}
