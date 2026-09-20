# Quick start

Ten minutes, no hardware required. Then, if you have a panel, the part that
touches it.

## 1. Install and run (2 minutes)

Requires Node 20 or newer.

```bash
git clone <this repository>
cd note4c-control-tower
npm install
npm run dev
```

Open <http://localhost:8654>. The port is bound to loopback; nothing else on
your network can reach it.

That is enforced rather than assumed: `npm run dev` runs
`tools/tower-serve.ts`, which applies the bind policy *before* Next starts. If
you point it somewhere else — `NOTE4C_TOWER_HOST`, or `npm run dev --
--hostname ...` — and the policy refuses the host, the command prints why and
exits without opening a socket. Binding every interface is
refused outright, even with a passphrase set and `NOTE4C_TOWER_ALLOW_LAN=1`.

## 2. Set a passphrase (1 minute)

The first screen asks you to choose one, twice. It protects the device token
and every push, and the server refuses to bind anything but loopback until one
exists. There is no account, no email, no recovery link: it is a local tool,
and forgetting the passphrase means deleting `~/.note4c-control-tower` and
starting over.

You land on **Overview**, with a short welcome card explaining that you are on
the simulated device. Every device-derived reading on the screen carries a
`SIMULATED` badge while that is true.

## 3. Make a composition (2 minutes)

Go to **Compositions** and press **+ New composition**. Five templates come up,
and each thumbnail is the real renderer running over the layout that button
would create, at the panel's four pigments — so what you pick is what you get.
Take **Morning info board**: six tiles on an 8×6 grid of 50-pixel cells,
already saved.

You land straight in the editor:

- Drag a tile to move it, drag its corner to resize. Both snap to the grid, and
  a move that would overlap simply does not land.
- From the keyboard: Tab to a tile, arrows to move it, Shift+arrows to resize
  it, Delete to remove it. Same rules, same refusals.
- On a phone, the canvas fits the width and the selected tile's options open in
  a sheet below it.
- The canvas is the real renderer. What you see is what would be sent.

Edit some words in the inspector, then press **Save**. Versions are
append-only; restoring an old one copies it forward rather than rewriting
history, and the whole list is behind the `v1 · saved` line under the title.

## 4. Show it on the simulated panel (1 minute)

Press **Show on panel**, from the editor or from the card in **Compositions**.
(The occasional actions — duplicate, archive — are behind the `⋯` button beside
it.)

A review opens with the exact frame that would be sent, rendered by a dry run
through the same pipeline the send uses. Confirm, and watch what happens,
because this is the whole product in one interaction:

- The frame is packed to exactly 30000 bytes and sent.
- The five stages are lit from the ledger, not from a timer: each one means a
  line was actually written down.
- The tower waits for the device to confirm it *displayed* it, not merely
  stored it, and only then says so — with the measured refresh time.

If you send the same picture twice, the second one is deduplicated and the
review says so before you press anything: repainting an unchanged frame costs a
full e-paper refresh cycle for nothing.

## 5. Look at the evidence (2 minutes)

**Advanced** is the page that makes the rest believable — four tabs: the event
log, the facts the device reported about itself, the send history with its
digests, and a Tower tab with the build version and data root. That last one is
the thing to paste into an issue.

## 6. Configure your own sources (2 minutes)

Nothing is configured by default, and every unconfigured source says so rather
than pretending to be broken. Copy the example file and fill in what you have:

```bash
cp .env.example .env.local
```

The most likely first one is the weather, which needs a coarse location:

```bash
NOTE4C_WEATHER_LATITUDE=45.50
NOTE4C_WEATHER_LONGITUDE=-73.57
NOTE4C_WEATHER_LABEL="City centre approx."
```

Use a city centre, not your address. The panel prints the label beside the
numbers as provenance, a coordinate pair in a config file ends up in backups,
and a forecast is the same for a whole city anyway.

Restart `npm run dev` after editing `.env.local`.

---

# The real panel

Everything above is reversible and touches nothing. The rest of this document
does touch hardware, so it is deliberate, gated, and worth reading first.

## What a push actually costs

A four-colour e-paper refresh takes **tens of seconds** of active panel time —
around 25 seconds measured on the device this was built against — during which
the panel flashes through its colour passes. E-paper has a finite number of
refreshes in it. The tower deduplicates identical frames for exactly this
reason, and it will tell you before every real push what it is about to spend.

## Before you start

You need three things:

1. The panel's **LAN address**, a private IPv4 literal (`192.168.x.y`,
   `10.x.y.z`, or `172.16–31.x.y`). Hostnames are refused on purpose: a name
   can be made to resolve somewhere else, and an IP literal cannot.
2. A **device token**, which the device mints when you pair with it.
3. The device **awake**. In automatic power saving it sleeps between refreshes
   and nothing over the network can wake it. Press the button on the device, or
   ask for an interactive window and wait for its next wake.

## Step 1: point the tower at the device

Device → **Real device**:

- Type the address. It is validated as private IPv4 before anything is stored.
- Provide the token. If you already have one in a file, set
  `NOTE4C_BRIDGE_TOKEN_PATH` to that file and an **Import the bridge token**
  button appears; it names the exact file it is about to read, copies the value
  into the tower's own secrets directory at mode 0600, and never displays it.
  Otherwise pair with the device to mint one.
- Press **Switch to the real panel** and type `REAL DEVICE` when asked.

The tower performs one **read-only status read** first. If the device does not
answer, it stays on the mock rather than claiming a connection it does not
have.

The `SIMULATED` badge disappears at this point. Everything on screen is now the
device's own answer.

## Step 2: understand what the device is doing

The state strip at the top of Overview and Device shows one word, from one
function, on every page:

| Word | What it means |
| --- | --- |
| `AWAKE` | It answered the last read. |
| `ASLEEP` | It did not answer, and that is the designed state between refreshes. Not a fault. |
| `PENDING` | Something is waiting to be delivered at its next wake, or the device says it has not taken up a mode yet. |
| `UNCERTAIN` | It is past the wake that was expected of it, or the tower has never reached it. |
| `UNREACHABLE` | It is silent when it owes an answer — set to stay awake, or in an interactive window. |

Before you change a setting, use **Interactive 15 min**. If the device is awake
the request is applied now; if it is asleep the request is *recorded* and
applied at its next wake, and the interface says which of those happened. It
never says "done".

## Step 3: one send

Press **Show on panel** and type `PUSH` when asked — the typed word is required
for a real device and for nothing else. Then watch the send history on
**Advanced**: `sent` → `stored` → `verified_displayed`, with the measured panel
time on the last line.

If confirmation never arrives, the send is `uncertain` and blocks later sends
until you re-check the device or accept the unknown outcome from Overview. That is not a bug to work
around: it is the tower refusing to tell you a frame is on the wall when it
does not know.

## Step 4: settings, if the firmware supports them

The Device page negotiates the firmware's `api` level and capability list. On a
firmware that exposes the typed configuration API, editable rows become live
controls and a bar appears at the bottom of the screen as soon as you change
something, with the count of pending changes and one **Apply** that writes them
all in a single request.

Every write carries the config revision the tower last read. If somebody
changed a setting on the device itself in the meantime, the device refuses the
write, and the tower shows you both values instead of retrying blindly.

On a firmware without it, every row says "on-device only" and explains why.
Nothing pretends.

## Getting back out

Device → Real device → **Back to the mock device**, and optionally **Forget the
token**, which deletes the tower's copy. The device is untouched by either.
