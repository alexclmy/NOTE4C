# Architecture

How a number becomes a pixel on a wall, and what refuses to happen along the
way.

## The shape of it

```
 browser (React, one origin, loopback)
    │  fetch, CSRF-echoed on every mutation
    ▼
 Next route handlers  app/api/**
    │
    ├── src/server/store/      atomic JSON documents under ~/.note4c-control-tower
    ├── src/server/sources/    weather, calendar, HA sensors, Reminders
    ├── src/core/render/       modules → frame buffer → pack() → 30000 bytes
    └── src/server/device/     address guard → HTTP client → ledger
                                        │
                                        ▼
                              the panel, on your LAN, port 80
```

There is no database, no message queue, no worker pool and no cloud. The state
is a handful of JSON files written atomically, and the only background job is a
refresh scheduler that is off unless a LaunchAgent turns it on.

## The renderer, and why the preview is the same code

`src/core/render/` is pure TypeScript with no Node API in it. It takes a
dashboard document and a bag of source values and produces a frame buffer of
palette indices, which `pack()` turns into exactly 30000 bytes: 400×300 pixels,
2 bits each, MSB first, black=0 white=1 yellow=2 red=3.

Because it is portable, the browser runs the identical module to paint the
designer canvas — palette indices straight to a canvas with nearest-neighbour
scaling. A preview built from HTML and CSS would be a different program
producing a similar-looking picture, which is exactly the class of bug that
puts a surprise on the wall.

`pack()` throws on any pixel outside the four-colour set. That throw is the
acceptance gate for everything this product renders.

### Modules

Each module in `src/core/render/modules/` declares a zod schema for its
options, a source binding, and a draw function. Two things fall out of that:

- The designer's inspector is **generated from the schema**, so the interface
  cannot offer an option the renderer would reject.
- The tower fetches only the sources that the modules on the selected dashboard
  actually bind to. A dashboard with no weather tile makes no weather request.

Every module receives `{state, value?, observedAt?, detail?}` and must draw its
own explicit unavailable or stale state. No module may substitute a zero.

### Themes and the palette

A dashboard theme may *narrow* the palette — switch off red or yellow — which
does not remove the pigment from the panel. It tells the renderer to stop
asking, and a final pass maps every request for a disabled colour to an enabled
one and reports how many pixels it remapped. Black and white cannot be switched
off; the frame would have nothing on it.

### Text

Text roles (`src/core/render/text.ts`) are the unit of editable content: the
words, whether they are shown, and how they are set. Text that does not fit its
tile is **marked in red on the panel** and reported to the designer with the
exact pixel numbers, rather than being cut silently. The designer's warning and
the panel's mark come from the same render, so they cannot disagree.

### Fonts

Glyphs are committed monochrome atlases (`src/core/render/fonts/*.json`),
rasterised from vendored OFL faces by `tools/gen_font_atlas.py`. Rendering
therefore needs no font installed and fetches nothing. Regeneration is
deterministic: unchanged input, byte-identical output.

## The push pipeline

`src/server/device/pushPipeline.ts`, and the order is the point:

1. **Collect** only the sources this dashboard binds to.
2. **Render and pack.** Compute two digests: the bytes' `sha256`, and a
   *semantic* hash of the content that ignores the clock.
3. **Deduplicate** against the last verified push, on the semantic hash. An
   identical picture is refused unless forced, because repainting costs a full
   refresh cycle.
4. **Gate.** An unresolved `uncertain` push blocks every later one. Real-device
   pushes require the typed word `PUSH`.
5. **Send**, then wait for the device to report it **displayed** the frame, not
   merely stored it.
6. **Record** in the append-only ledger: digest, dashboard, version, state, and
   the measured panel time.

The ledger is also the map back: a digest the device reports is resolved to the
dashboard version that produced it. A digest with no entry is an *unknown
frame*, said in those words.

Ledger states: `sent` → `stored` → `verified_displayed`, or `failed`, or
`uncertain` when no confirmation arrives. `uncertain` is not a synonym for
failed. It means the tower does not know, and it will not guess.

### The transport, and why it is not `fetch`

`src/server/device/transport.ts` opens the socket, on `node:http`, and that
choice is a fix rather than a preference. `fetch` is a *global*: the Next.js
server replaces it with an instrumented version and shares one process-wide
connection pool with everything else in the process. A panel that answered in
70 ms from a one-shot script was reported unreachable from inside the running
server for exactly that reason. So the transport takes `http.request` off the
module object, gives every call its own socket with `Connection: close` —
the panel's four sockets are not the tower's to pool — and asserts the address
and the path allowlist at the line that connects rather than only in its
caller.

A failed call is then *classified*, in `src/server/device/failure.ts`, because
`reachable: false` was carrying two unrelated facts:

| kind | what it means |
| --- | --- |
| `absent` | nothing answered. A sleeping panel, a powered-off one and a dropped link are indistinguishable, and this is the normal state. Not notable, nothing recorded. |
| `transport` | the device sent a status line and then stopped, or the tower's own transport failed without touching the wire. A fault. |
| `refused` | the device answered and said no. |
| `contract` | the device answered off-contract. |
| `uncertain` | the deadline ran out. The tower does not know. |
| `not_configured` | no token, or an address the tower will not dial. |

Only the deadline produces `uncertain`, and only `absent` is silent. That
split is what the push pipeline's retry and queue decisions rest on.

## The power model

`src/core/power.ts`, and it exists because of one fact: **a sleeping device
cannot be woken over Wi-Fi.** The radio is off.

- A mode change aimed at a sleeping device is written down as a `PowerIntent`
  in the tower's durable state, with what was asked for and when.
- `planIntent()` is the single decision shared by the status route, the
  `POST /api/device/power` handler and the background scheduler, so the
  sentence the user reads and the action the tower takes cannot disagree.
- An interactive intent expires after six hours: "make it interactive" is a
  request about *now*, and waking a device into an interactive window long
  after the person gave up would spend battery for nobody.
- `deriveDeviceState()` turns a reading into one of five words —
  `awake`, `asleep`, `pending`, `uncertain`, `unreachable` — with the reason in
  plain language. Every page renders that one function's answer.

Reads do not write. `GET /api/device/status` used to deliver pending intents,
which made a GET able to reconfigure hardware; delivery now happens only on the
POST route (session + CSRF) and on the scheduler.

## Storage

`src/server/store/atomicFile.ts` writes every document through a temp file and
a rename, with a schema version and a migration hook. Reading a document of an
older version migrates it forward, backs up the original, and rewrites it. A
malformed document is never partially applied.

The data root is created at mode 0700 outside the repository, secrets at 0700
with files at 0600.

## Security boundaries

Stated as properties, each enforced in one place:

- **Bind.** Loopback only, unless a passphrase is set *and*
  `NOTE4C_TOWER_ALLOW_LAN=1`. Binding all interfaces is refused outright, even
  when both of those hold. The policy is one pure function
  (`src/server/auth/bindGuard.ts`) with two enforcers: `tools/tower-serve.ts`,
  which `npm run dev` and `npm start` go through and which applies it *before*
  Next exists, so a refused host means no socket is ever opened; and
  `src/server/auth/bindStartup.ts`, called from `instrumentation.ts`, which
  looks at what this process is actually listening on and stops serving if the
  policy refuses it. The second one reports its own confidence — `GET
  /api/diagnostics` renders `undetermined` when it could not observe the
  socket, rather than reporting a loopback bind it never verified.
- **Auth.** One passphrase, scrypt with per-install parameters recorded beside
  the hash, signed session cookie, CSRF token echoed by every mutation.
  (`src/server/auth/`)
- **Device address.** Private RFC1918 IPv4 literals only; no hostnames, so DNS
  rebinding is not in the threat model. Port pinned to 80, a closed path
  allowlist (`ALLOWED_PATHS`, a fixed list rather than a prefix rule),
  redirects never followed. Decided in `src/server/device/address.ts` and
  re-asserted in `src/server/device/transport.ts`, at the line that opens the
  socket, so the guarantee belongs to the code that connects.
- **Concurrency.** One single-flight mutex around device calls, because the
  device serves at most four sockets and its own UI is a client of the same
  server.
- **Secrets.** Server-side only, write-only through the interface. The device
  token, the voice hub token and any Home Assistant token never appear in a
  response, the ledger, the audit log, or the page. The audit writer redacts
  any parameter whose name looks like a credential.

## The interface

`src/ui/` is a small design system rather than a component library:

- `tokens.css` — ink on cream paper, 2 px borders, hard shadows with no blur,
  zero radius. Red means destructive, failed or unvouched-for; yellow is the
  primary action colour *and* the colour of pending, queued and stale. Nothing
  else may use either. Dashed borders mean provisional. Every duration is a
  `--motion-*` variable, so `prefers-reduced-motion` is honoured in one place.
- `fonts.ts` — Space Grotesk for everything, IBM Plex Mono for labels and
  values, both OFL and both self-hosted at build time. No CDN, ever; the
  screenshot QA fails on any external request.
- `AppShell.tsx` — a sticky 60 px header carrying the brand and the device
  chip, four navigation entries under it on a desktop and a fixed tab bar at
  the bottom of a phone. Exactly one `nav aria-label="Main"` is in the
  accessibility tree at any width, because the other is `display: none`.
- `components.tsx` — Badge, Card, Row, Button, Cta, Banner, MonoLabel,
  TechDetails, FreshnessStamp. A `Row` with a hint makes its *label* the
  control that reveals it, because an inline "i" glyph is not a touch target
  and a `title` attribute is invisible to a finger.
- `Dialog.tsx` — focus trap, Escape, focus restoration, scroll lock. One
  overlay at a time, enforced by a module-level registry that throws in
  development (`overlay.ts`). A toast is not an overlay and does not register.
- `DeviceChip.tsx` and `DeviceStateBadge.tsx` — two renderings of one word,
  both from `deriveDeviceState`, so the header and the page cannot disagree.
- `SendFlowDialog.tsx` — the one place a person deliberately reaches the panel.
  Its five progress stages are lit from the push ledger (`GET /api/device/push`,
  read-only, local) rather than from a timer; the mapping is `sendStages.ts`
  and it is unit-tested, because "a stage is lit because a line was written"
  is the promise the whole screen rests on.
- `ScaledPanel.tsx` — the 400 × 300 panel at whatever size its column allows.
  A `transform`, never a width: what goes inside is either the renderer's own
  canvas or a PNG the device produced, and both have to stay exactly 400 × 300.
- `StateStrip.tsx`, `StickyActions.tsx`, `BottomSheet.tsx`, `PageState.tsx`,
  `Toast.tsx` — the patterns that make the phone layout work.

## Testing

- **vitest** for everything pure: the renderer, `pack()`, the power model, the
  schema migrations, the source adapters against fixtures. The suite pins a
  panel timezone so rendering assertions do not depend on who ran them.
- **Playwright** across three viewports (1440×900, 375×812, 768×1024), always
  against the in-repo mock, with its own port and its own throwaway data root.
  It can never touch a real device or your real store.
- Browser runs use fixture source data (`NOTE4C_TOWER_E2E=1`), so a suite about
  the user interface never reaches the network, the calendar or Reminders.

## The mock

`mock-device/` implements the device's HTTP contract faithfully, including the
things that make a tower hard to write: a configurable panel delay, an
acknowledgement that can fail, a config revision that can move underneath you,
a device that stops answering, and a battery the ADC cannot honestly measure.
Most of the awkward states this product renders exist because the mock can
produce them.
