/**
 * Typed confirmation words for anything that reaches the physical panel.
 *
 * Shared between the server routes that enforce them and the dialogs that ask
 * for them, so the word the user is told to type is by construction the word
 * the server will accept.
 */

/** Sending a frame to the real device. */
export const REAL_PUSH_CONFIRMATION = "PUSH";

/** Pointing the tower at the real device instead of the mock. */
export const REAL_MODE_CONFIRMATION = "REAL DEVICE";

/**
 * The composer bridge credential used to be named here, as an absolute path
 * into one person's home directory. It is now NOTE4C_BRIDGE_TOKEN_PATH, read
 * on the server by src/server/config.ts, and handed to the browser by
 * GET /api/state so the consent dialog can name the exact file it is about to
 * read. This file holds only the words a user types, which are the same on
 * every machine.
 */
