# Contributing

Thank you for looking. This is a small, opinionated project for one specific
piece of hardware, and the opinions are most of the value — so this document
spends more words on *how* things are written here than on git mechanics.

## Getting set up

```bash
npm install
npm run dev          # http://localhost:8654, against the in-repo mock
                     # (goes through tools/tower-serve.ts, the bind guard)
```

Node 20 or newer. No hardware needed: the mock speaks the device's contract,
and the whole test suite runs against it.

```bash
cp .env.example .env.local   # optional: your own sources
```

## The gates

Everything below must pass before a pull request is ready. They are the same
commands CI runs.

```bash
npm run typecheck    # tsc --noEmit, no errors, no suppressions
npm run test         # vitest
npm run build:check  # a real production build, into a scratch directory
npm run test:e2e     # Playwright, three viewports, against the mock
```

`build:check` rather than `build`: a plain `next build` writes into the `.next`
a running server reads from, and rewrites `next-env.d.ts` and `tsconfig.json`
on its way through. `tools/build-check.sh` builds somewhere harmless and puts
those two files back.

The browser suite takes a few minutes. It uses its own port, its own throwaway
data root under the system temp directory, and the in-repo mock — it can never
reach a real device or your real store.

It runs against a **production build**, not `next dev`: `tools/e2eServer.ts`
runs `next build` into the `.next-e2e` scratch directory and then `next start`
on the suite's loopback port, and Playwright waits for that. The development
server compiles routes on demand and occasionally reloads an open page to
recover a module graph it could not patch ("Fast Refresh had to perform a full
reload"), which lands as a page reload in the middle of whatever spec is
running; that was a real, intermittent, one-test-per-run failure. The build
costs half a minute at the head of the run — less when the scratch tree's cache
is warm, and the tree is deliberately left on disk between runs — and in
exchange nothing recompiles while the browser is driving.

## House style

### Comments explain *why*, and they are part of the change

This codebase has an unusually high comment density, on purpose. The rule is
not "comment everything"; it is that a decision which cost thought should not
have to be re-derived by the next reader. A good comment here says what was
tried, what went wrong, or what would break if the code were written the
obvious way. Look at `src/core/power.ts` or `src/ui/Dialog.tsx` for the register.

A comment that restates the code is worse than none. A comment that records a
defect the code exists to prevent is the most valuable line in the file.

### Honesty is a functional requirement

This is the thing to internalise before changing any user-facing behaviour.
The product's subject is a device that is asleep most of the time and a display
that holds a wrong number for hours. So:

- Never render a value the tower does not have. No zeros, no dashes, no
  placeholders standing in for missing data. Say what is missing and why.
- Never report success the device has not confirmed. "Sent" is not
  "displayed"; "requested" is not "applied".
- Never offer a control for something that cannot happen. There is no "wake the
  device" button, and the note explaining why sits where that button would be.
- Distinguish "not configured" from "failed". They send the reader to two
  different places.
- Keep the state words exact. `awake`, `asleep`, `pending`, `uncertain`,
  `unreachable` are computed in one place — `deriveDeviceState()` — and no page
  decides for itself.

### Visual language

Ink on cream paper. Every surface is bordered in **2 px of ink**, every raised
thing casts a **hard shadow with no blur**, and nothing has a corner radius. A
press moves the object into its own shadow rather than tinting it. No
gradients. The grammar is deliberately the opposite of the soft, radiused,
shadowless chrome this section used to describe: the panel this tower drives is
four flat pigments behind a hard bezel, and the interface around it now says so.

**Red means destructive, failed, or a state the tower cannot vouch for.
Yellow is the primary action colour *and* the colour of pending, queued and
stale.** The two jobs yellow does never appear in the same role — one filled
yellow button per view, and yellow fills for things that are waiting — and no
third meaning may be added to either colour. Nothing else gets colour at all.

State semantics, which are the same everywhere a device state is drawn (see
`data-device-state` in `src/ui/app.css`):

| State | Drawn as |
| --- | --- |
| awake | ink, still |
| asleep | yellow, still |
| pending | yellow, pulsing |
| uncertain | red, pulsing |
| unreachable | red, still |
| simulated | a full-bleed yellow banner |

A **dashed** 2 px border means provisional: an experiment that has never been
validated on hardware, or a page nobody needs day to day. It is the one way to
say "not finished" without spending a colour on it.

Type is Space Grotesk for everything, IBM Plex Mono for labels, timestamps and
technical values. Text that carries prose is at least 12 px; spaced mono
capitals may go to 10 px, and only ever as a label sitting beside the thing it
names. Touch targets are at least 44 px.

Motion is opacity or translation, read from the `--motion-*` tokens and never
hard-coded, so `prefers-reduced-motion` turns all of it off in one place. Two
animations exist: `rise` says where something came from, `pulse` says something
is still happening. A pulse is only ever used for a state that is genuinely
unresolved.

Never mount two overlays at once. `src/ui/overlay.ts` enforces it and throws in
development if you try.

### Dependencies

Four runtime dependencies: `next`, `react`, `react-dom`, `zod`. Adding a fifth
needs an argument in the pull request, not just a package name. A forty-line
focus trap that we understand beats a dependency that we do not.

### Nothing at runtime from the network

The browser fetches no third-party asset — no font CDN, no analytics, no
telemetry. Fonts are vendored and self-hosted; glyph atlases are committed
bitmaps. The only outbound calls are the optional weather sources, and they are
documented in `NOTICE` and in the README.

### Tests

New behaviour arrives with tests, and preferably before it:

- Pure logic → vitest. If it needs a running server to test, consider whether
  it should be pure.
- User-visible behaviour → a Playwright spec. Keep `data-testid` attributes
  stable; they are a contract with the suite. If one has to move, move the
  test in the same change.
- A bug fix arrives with the test that would have caught it.

## Making a change

1. Open an issue first for anything larger than a fix, so the design can be
   argued before the code is written.
2. Branch, write the change with its tests, run the four gates.
3. Commit messages in the imperative, describing the change and its reason:
   *"Retry dashboard refresh after device outages"*.
4. Open a pull request using the template. Say what you ran, and say what you
   did not run — especially whether anything touched real hardware.

## What is out of scope

- **Support for other panels.** See `docs/COMPATIBILITY.md`. The renderer is
  built around one frame format and one API, deliberately.
- **Cloud anything.** No accounts, no sync, no remote access. This is a program
  that runs on the machine in front of you.
- **A prettier preview.** The preview is the renderer. That is not negotiable:
  a second rendering path is a second opinion about what is on the wall.

## Hardware claims

If you change something on the real-device path, say in the pull request
exactly what you executed on hardware and what you only exercised against the
mock. "It should work on the device" is fine to say; writing it as if it had
been tested is not.
