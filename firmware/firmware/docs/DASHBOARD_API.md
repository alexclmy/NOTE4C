# Dashboard API v1

All routes live on the HTTP server the device already runs on its LAN
interface. This project opens no new listening socket, on the device or on the
Mac.

Base path: `/api/v1/dashboard`

This document describes the frame API, which is unchanged. The device now also
reports `"api": 2` and serves a typed settings surface and two device actions;
those are in `CONFIG_API.md`. The paths stay under `/api/v1/` because the frame
contract did not change and renaming it would break a working bridge for no
reason.

This firmware builds with `DASHBOARD_MINIMAL_UI=ON`, which removes the upstream
content pages and their third-party data fetchers from the binary. See
`main/dashboard_build_config.h`.

## Direction of travel

The Mac pushes; the device never pulls.

This is not the arrangement the original plan described. That plan had the
device fetch frames from a server on the Mac, but the Mac's composer listens on
`localhost` and on the tailnet, and the device — an ordinary LAN client — can
reach neither. Making the pull work would require binding a new port on a LAN
interface of the Mac, which was not authorised. Pushing to a route on the
device's existing server achieves the same result and adds no new exposure.

The honest consequence: **the device cannot request a frame.** Its front button
repaints the frame already stored on it. The firmware does not pretend
otherwise, and neither should any UI built on this API.

## Authentication

Every mutating route requires `X-Auth-Token`, a 64-character lowercase hex
string. See `PROVISIONING.md` for how it gets there.

A device that has never been paired refuses **all** mutating routes with
`503 not_provisioned`. There is no default token, no fallback and no route that
installs one from the network outside a physically opened pairing window.

Ten failed authentications inside sixty seconds trigger `429 locked_out` for the
remainder of that window. A correct token does not bypass the lockout.

## Legacy routes

The pre-existing gallery routes (`POST /upload`, `DELETE /photo`,
`POST /settings`, `POST /photo/show`, `POST /photo/meta`, `POST /photos/move`)
accept writes from anyone who can reach the device on the LAN. Adding an
authenticated API beside them would not make the device secure while they stay
open, so this firmware refuses them with `403 lockdown` by default.

Read routes (`GET /`, `GET /status`, `GET /photos`, `GET /photo`,
`GET /settings`) are unchanged and remain unauthenticated.

The gate can be turned off in **Settings → Block legacy writes**. Doing so
restores the upstream behaviour, including its exposure. That is a deliberate
user choice, not a default.

---

## `PUT /api/v1/dashboard/frame`

Store a new frame.

| Header | Required | Meaning |
| --- | --- | --- |
| `Content-Length` | yes | must be exactly `30000` |
| `X-Auth-Token` | yes | device token |
| `X-Frame-Sha256` | recommended | SHA-256 of the body, hex. Checked before storing. |
| `Idempotency-Key` | recommended | retry-safe key; a repeat is not repainted |
| `X-Frame-Epoch` | optional | Unix seconds, stored as informational metadata |

Body: 30000 raw bytes, 400x300 at 2 bits per pixel, MSB first,
`black=0 white=1 yellow=2 red=3` — the panel's own layout, so the device copies
it without conversion.

### Responses

| Status | Meaning |
| --- | --- |
| `202` | New frame stored and verified; a repaint has started or is queued |
| `200` | `deduped` (identical bytes already stored) or `replay` (key already applied) |
| `400` | Wrong length, or the body ended early |
| `401` | Missing or wrong token |
| `403` | — (not used by this route) |
| `408` | Timed out reading the body |
| `409` | Another mutating request is already in flight |
| `413` | `Content-Length` greater than 30000 |
| `422` | Body does not match `X-Frame-Sha256` |
| `429` | Authentication lockout in effect |
| `500` | Write or read-back verification failed; the previous frame is still active |
| `503` | Device not paired, or storage unavailable |

Success body:

```json
{"accepted":true,"persisted":true,"deduped":false,"replay":false,
 "seq":42,"sha256":"<hex>","render":"started"}
```

**`accepted`, `persisted` and `render` mean three different things**, and are
reported separately because conflating them is how callers end up believing a
frame is on the glass when it is not:

- `accepted` — the request was well-formed and authorised.
- `persisted` — the bytes are in flash *and were read back and re-verified*.
- `render` — `started`, `queued`, `coalesced`, or `skipped`. A panel refresh
  takes many seconds; when this response is sent, it has almost certainly not
  finished.

To know what is actually displayed, read `displayed.sha256` from the status
route. To know what is actually stored, read it back with `GET .../frame`.

## `GET /api/v1/dashboard/frame`

Returns the stored frame, 30000 bytes, with `X-Frame-Sha256`. The record's
checksums are re-verified on the way out, so this is a genuine read-back rather
than an echo of what was sent. `404` when nothing is stored.

No token required: it returns a picture that is already on public display on the
front of the device.

## `GET /api/v1/dashboard/status`

```json
{
  "firmware": "Marvin 0.2", "api": 2,
  "capabilities": ["dashboard.frame.v1", "config.v2", "action.restart", "..."],
  "device": {"name": "Poulailler Terminal", "model": "zectrix-s3-epaper-4.2",
             "hardware": "NOTE4C 4-color", "fw": "Marvin 0.2",
             "upstream_base": "6.5.9"},
  "config_revision": 7,
  "initialised": true, "provisioned": true, "lockdown": true,
  "stored":    {"present": true, "seq": 42, "sha256": "<hex>", "source_epoch": 1757500000},
  "displayed": {"present": true, "seq": 41, "sha256": "<hex>"},
  "refresh":   {"state": "rendering", "pending": false,
                "renders": 17, "skipped": 4, "coalesced": 9,
                "failed": 0, "last_failed": false, "last_failed_seq": 0,
                "deferred": 2, "last_deferred": false, "last_deferred_seq": 0},
  "timing_ms": {"read": 3, "blit": 2, "panel": 14200, "total": 14205},
  "storage":   {"write_failures": 0, "read_failures": 0,
                "spiffs_total": 7929856, "spiffs_used": 131072}
}
```

`storage.write_failures` and `storage.read_failures` are non-zero when the
filesystem is refusing or losing writes. Watch them: a device that is quietly
failing to persist frames still repaints the panel with what it already had, so
the symptom is a dashboard that stops updating rather than an obvious error.

`stored` and `displayed` differ whenever a refresh is in flight or the user is
looking at another page. That difference is the point of reporting both.

`refresh.failed`, `refresh.last_failed` and `refresh.last_failed_seq` cover the
case where the two never converge because a render started and **faulted**: the
panel was asked to draw and came back without ever releasing BUSY. Before these
fields the only evidence was `refresh.renders` failing to advance, which is also
what a device nobody pushed to looks like — so a client waiting for its digest
to appear under `displayed` had no way to tell "still coming" from "dropped".
`failed` is a total since boot, `last_failed` describes only the most recent
completed render and is cleared by the next one that draws or is deferred, and
`last_failed_seq` names the sequence that faulted (it is history, and is not
cleared by a later success).

`refresh.deferred`, `refresh.last_deferred` and `refresh.last_deferred_seq` are
the **benign twin** of the `failed` trio. A frame that arrives while the user is
on another page is stored and deliberately *not* drawn — it appears the next
time the dashboard is opened. That is expected behaviour, not a fault. It used
to increment `failed`, so a tower could not tell a genuinely dropped frame from
a user simply reading a different page; now that case increments `deferred`
instead and `failed` counts **only** faults. A deferral never changes
`displayed`. `deferred` is a total since boot, `last_deferred` describes only
the most recent completed render, and `last_deferred_seq` names the sequence
that was held back.

All six keys are additive. A client written against the earlier response keeps
working: nothing was renamed or removed. The one refinement is that `failed` no
longer counts the benign "another page is open" case — if you were treating a
rising `failed` as a fault signal, it is now a more accurate one.

`api`, `capabilities`, `device` and `config_revision` are the api 2 additions.
An api 1 device omits all four; a client that reads `api` first and branches on
`capabilities` never has to guess. See `CONFIG_API.md`.

The `stored` and `displayed` objects themselves are **unchanged** by the autonomy
work below. Provenance — whether the frame was pushed or composed here — is
reported in the `autonomy` block instead, not repeated in these two, because a
fact with two homes is a fact that will eventually disagree with itself.

### Where this response is built, and the one time it fails

The response is assembled in a single ~4.5 KB block — the capability list, the
`power` block, the `autonomy` block and the document itself — taken from PSRAM
for the duration of the request and released before the handler returns. It is
**not** built on the stack, and that is a correctness requirement rather than a
preference.

It used to be. The four buffers were locals, the compiled frame was 4 976 bytes
inside an httpd task given 6 144, and one unauthenticated `GET` was enough to
run the task's stack past its limit and over the logging mutex's task control
block. The device did not return an error; it asserted in
`xTaskPriorityDisinherit` and reset. Raising the task's stack was rejected: every
other route on this server lives inside the same 6 144 bytes, and none of them
should pay for this one.

So this route can now answer `503 {"error":"no_memory"}` where it previously
could only answer `200`. It does so when neither PSRAM nor the internal heap can
supply the block — the error body is small and formatted on the stack, which is
what makes "out of memory" something the device can still say out loud. Treat it
the way you would a `503` anywhere else here: transient, retryable, and not a
statement that the device lacks the capabilities it advertised earlier.

### Reading `timing_ms`

- `read` — loading the record from SPIFFS and re-verifying its SHA-256.
- `blit` — copying 30000 bytes into the framebuffer.
- `panel` — the whole handshake: trigger, driver transfer, and the wait on the
  BUSY pin.

`panel` dominates, typically by three orders of magnitude. That time is spent
inside the panel developing four pigments, and **no change to this firmware
shortens it**. This project makes no attempt to alter waveforms or drive
voltages: those are undocumented for this part, and getting them wrong damages
panels permanently.

What *is* reduced is the number of refreshes. Identical frames are detected by
digest and skipped outright (`refresh.skipped`), and bursts collapse into one
repaint (`refresh.coalesced`). Those are real savings; a faster refresh is not
available and is not claimed.

## `POST /api/v1/dashboard/refresh`

Force a repaint of the stored frame, even if the panel already shows it.
Requires `X-Auth-Token` — a repaint costs seconds of hardware time and e-paper
wear, so it is treated as a mutating operation.

`202` when a repaint started, `200` when it was queued or coalesced,
`404` when nothing is stored.

## `POST /api/v1/dashboard/pair`

Claims the token minted by a pairing window opened on the device.
Carries no token — it is the bootstrap. See `PROVISIONING.md`.

`200 {"token":"<hex>"}` on success, `403 not_pairing` otherwise.

---

## Concurrency

One mutating request is served at a time; a second gets `409` rather than
queuing behind the first, so a client always knows whether its frame was
applied.

Panel refreshes are single-flight with depth-1 coalescing. While a refresh runs,
the first new request is marked pending and every later one folds into it. When
the refresh finishes, the pending repaint renders **whatever is stored at that
moment** — so a burst of six frames produces two refreshes and ends on frame
six, not on frame two.

The HTTP task never blocks on the panel. Rendering happens on a separate task,
and the store's lock is released before the refresh begins.

### How a refresh is acknowledged

The driver's `read_busy()` waits up to 120 s for the BUSY pin, then gives up and
lets the refresh continue anyway; `EPD_TurnOnDisplay()` calls it three times. So
"the refresh task returned" does not mean "the panel drew the frame", and one
refresh can legitimately occupy the driver for about six minutes.

A frame is therefore only recorded as displayed when **both** hold:

1. the driver signalled completion (a real semaphore, not a polled flag), and
2. no BUSY-pin timeout occurred during that refresh.

Otherwise `displayed.sha256` keeps reporting the previous frame, which is the
one still believed to be on the glass. Stale completion signals are drained
before each refresh is triggered, the wait is always bounded, and overlapping
handshakes are refused rather than interleaved. The rules live in
`dashboard::RenderHandshake` and are covered by host tests.

If BUSY never releases, the panel contents are genuinely unknown. The firmware
reports that; it cannot fix it.

---

## Autonomy (off by default)

Three routes plus a status block, present only in a build with
`CONFIG_AUTONOMY_ENABLED=y`. On the
default build they answer `404 autonomy_unsupported` and the capability list
omits `autonomy.profile.v1`, so a tower reads the gap structurally rather than
offering controls that would do nothing.

There are two switches and both must be open:

| Switch | Kind | What it decides | Default |
| --- | --- | --- | --- |
| `CONFIG_AUTONOMY_ENABLED` | Kconfig, build time | whether the feature can be turned on at all | `n` |
| `autonomy.enabled` | config API v2, runtime | whether it is on right now | `false` |

`autonomy.enabled` **is persisted** (apply mode `immediate`, NVS namespace
`autonomy`). It was not, while a local render could only happen because somebody
asked for one — a bench enable did not outlive the bench. The wake cycle changed
that: it reaches the panel by going through deep sleep, which is a reboot, so an
unpersisted switch would come back off on the first wake and the feature could
never run unattended. The default is still `false`; persistence remembers a
decision, it does not make one. See `CONFIG_API.md`.

All three routes carry the dashboard token in `X-Auth-Token` and share the frame
route's failure lockout. There is no unauthenticated route here.

### `PUT /api/v1/autonomy/profile`

Body is the profile document, at most 16384 bytes. Headers:

- `X-Profile-Sha256` — optional; when present the body must hash to it.
- `Idempotency-Key` — optional; a ring of eight keys is remembered, so a retried
  PUT is answered with its original result marked `"replay": true` rather than
  applied twice.

The revision is a compare-and-swap: a revision that does not advance past the
stored one is `409 revision_mismatch`, and the detail names the revision the
device is holding so the tower recompiles past it rather than guessing.

The document is parsed and validated in full **before** a byte is written. A
profile that cannot be drawn is refused with the offending field named
(`400 invalid_profile`, detail `<code> at modules[2].rows[7]`), never stored for
later: a stored document that cannot be drawn is a panel that fails at 3am on
battery rather than while somebody was looking at the tower.

`200 {"accepted":true,"persisted":…,"replay":…,"revision":…,"sha256":"…"}`.

Other refusals: `413 profile_too_large`, `422 sha_mismatch`,
`500 store_failed`, plus the shared `401` / `429` / `503 not_provisioned`.

### `GET /api/v1/autonomy/profile`

Returns the stored bytes **verbatim**, with the digest in `X-Profile-Sha256`.
Verbatim is the contract: the tower compares what it pushed against what comes
back, byte for byte, and re-serialising here would make that a test of this
device's JSON writer instead of a test of what it holds.

`404 no_profile` when nothing is stored, which is the default state and not an
error.

### `POST /api/v1/autonomy/render`

Composes one panel from the stored profile, now, and hands it to the frame
store. **Pull, and it stays pull even now that a wake cycle exists:** the cycle
composes on its own schedule and this route composes because somebody holding
the token asked, at this moment, and wants to be told what happened to the frame.
It is the bench path — it does not wait out a wake interval.

Both go through the same compositor in `Application`, so what this route reports
is what the cycle would have drawn, and neither can run while the other is (one
canvas, one owner; the loser is told `409 busy` rather than queued).

What this route does **not** do is fetch. It draws from whatever forecast the
cache already holds, and reports `had_forecast` so the caller can tell a fresh
panel from one drawn with the weather module unavailable.

Submission goes through `DashboardManager::SubmitLocal()`, which differs from a
tower push in exactly one way that matters: it carries the sequence the store
held before the composition began, and the check is a compare-and-swap under the
store's own lock. A push that lands during the seconds of composing moves that
sequence, and the local frame is then dropped rather than written over the
operator's. **The tower always wins**, reported as `409 superseded` and not as a
failure — it is the arbitration rule working.

```json
{"composed":true,"outcome":"accepted","persisted":true,"modules_drawn":4,
 "empty":false,"expected_seq":14,"seq":15,"sha256":"…","render":"started",
 "had_forecast":true,"degraded":false}
```

`had_forecast` and `degraded` replace the `forecast` key, which was always
`null`, and the `degraded` flag, which was always hard-coded `true` because this
build had no forecast client at all. Both are now measurements:
`had_forecast` is whether the cache held a forecast to draw from, and `degraded`
follows it — a panel whose weather module is blank says so rather than looking
complete. A client reading the old `forecast` key gets `undefined`; the two
booleans carry the same information and more.

`outcome` is `accepted` (202), `deduped` (200, the panel already shows exactly
these bytes — no flash write and no 25-second refresh), `superseded` (409) or
`busy` (409).

Refusals name which gate is shut: `404 autonomy_unsupported` (build),
`409 autonomy_disabled` (runtime switch), `404 no_profile`,
`503 no_memory` (no PSRAM for the 120 KB canvas), `409 busy`.

### The `autonomy` block on `GET /api/v1/dashboard/status`

Additive, and `null` when this build has no profile store — so an older or
tighter firmware never breaks a tower's status parse. Present and honest
otherwise, including when autonomy is switched off.

```jsonc
"autonomy": {
  "enabled": true,
  "profile": {"present": true, "sha256": "…", "revision": 12,
              "profile_version": 1, "applied_epoch": 1789500000},
  "last_cycle": {
    "origin": "tower" | "local" | "none" | "unknown",
    "outcome": "updated" | "unchanged" | "degraded" | "failed",
    "rendered_epoch": 1789500000,
    "wifi": {"connected": true, "duration_ms": 4200},
    "fetch": {"weather": {"attempted": true, "ok": true, "http_status": 200,
                          "duration_ms": 1900, "bytes": 8123,
                          "fetched_epoch": 1789499000, "cache_age_s": 1000}}
  },
  "stored_origin": "tower" | "local" | "none",
  "displayed_origin": "tower" | "local" | "unknown" | "none",
  "next_wake_epoch": 1789503600
}
```

The claims this block makes, each pinned by a host test that reads the rendered
bytes back (`tests/host/test_autonomy_status.cc`):

- **An age is never a number when the clock was not set.** Every epoch is `null`
  instead. A fabricated timestamp on frozen ink is the exact failure this device
  refuses.
- **`degraded` is its own outcome and is never folded into `updated`.** It means
  the panel *was* drawn, from a cache or with a source missing.
- **`last_cycle` is `null` until a cycle has actually finished**, rather than a
  plausible default on a device that has just booted.
- **No part of the profile's content appears here** — not a module, not a string,
  not the coordinates. The tower already has the document it pushed and can read
  it back verbatim; repeating it here would put the panel's text in every status
  poll and every log line that captured one.
- **`http_status` is `-1` when no response arrived at all**, which is a different
  field report from a 500.

#### Two origins, and why they are not the same field

`stored_origin` is read from the flags word in the header of the record the A/B
store has active (`STORAGE.md`). It is always answerable, and it survives a deep
sleep because it is in flash rather than in RAM.

`displayed_origin` is the weaker claim, and deliberately separate. It is only
`tower` or `local` when this boot has seen a refresh complete **and** the frame
it completed is still the record the store holds — same sequence, same digest.
Otherwise it is `unknown`. That is the honest answer after a deep sleep: e-paper
keeps its image across the reboot while the coordinator that knows what was drawn
does not. A client must not fall back to `stored_origin` when this is `unknown`;
that is a different frame's provenance.

### The wake cycle, and the one outbound origin

With both switches open the device may, on a timer wake, compose a panel for
itself and make **at most one** HTTPS request per wake to
`https://api.open-meteo.com` for a forecast. The rules, all host tested:

- The origin is a **compile-time constant** and is checked against an exact host
  match immediately before every request. The profile document has **no URL
  field at any depth**, by design — adding one would be the single change that
  turns it from a document into an SSRF surface.
- **Zero redirects.** A redirect is an instruction to contact a different host,
  which is the thing the allowlist exists to prevent. Refused rather than
  followed-and-checked, because following it has already leaked the request.
- **32 KB response ceiling**, 20-second whole-operation deadline enforced at the
  socket on every read, **no API key**, **no retry inside one wake** — the next
  wake is the retry, with the backoff that already exists.
- A tower push **always wins immediately** over a local composition, and is never
  queued behind one. See `autonomy_policy.h` for the five arbitration rules in
  the order they are applied.

Every path through the cycle arms the next wake and sleeps; see `POWER.md` for
the budget arithmetic and for what is and is not actually bounded.

### Still not in this build

No microphone-driven or always-listening source, and no third-party integration
beyond the single forecast origin above and the NTP pools the clock already
used.
