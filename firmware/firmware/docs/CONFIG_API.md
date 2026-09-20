# Device API v2: configuration and actions

The frame API in `DASHBOARD_API.md` is unchanged and keeps its `/api/v1/`
paths. This document describes what api level 2 adds: a typed settings surface
and two device actions. Both live on the same HTTP server, use the same token,
and share the same failure lockout.

Contract source: `main/common/device_config.h` holds every rule and is compiled
directly by `tests/host/test_device_config.cc`. `main/ui/renderers/rawdraw/config_api.cc`
is the HTTP layer over it and contains no rule of its own.

## Why there is an api level at all

`firmware` is a version string for a human. It does not move in step with what
the device can do, and a client that branched on it would be guessing. `api` is
the contract number, and `capabilities` is the list of parts of that contract
this particular build compiled. A build with the microphone path compiled out
reports api 2 and omits `voice.ptt.v1`, which is how a client tells "there is
no capture code in this binary" apart from "the microphone is muted". Those
need different things from the user, so they are different answers.

`autonomy.profile.v1` works the same way. It is present only on a build with
`CONFIG_AUTONOMY_ENABLED=y`, where the three `/api/v1/autonomy/` routes exist
and the `autonomy.enabled` field below does something; on the default build the
string is absent and those routes answer `404 autonomy_unsupported`. Out of the
*list*, precisely: the literal is still in the binary's `.rodata`, because the
function that would emit it is still linked. The gate is over reachability
rather than presence — `strings` on an autonomy-off image finds
`autonomy.profile.v1` and a client reading the capability list does not.

## Discovery

`GET /api/v1/dashboard/status` is unauthenticated and gains three fields:

```json
{
  "firmware": "Marvin 0.2",
  "api": 2,
  "capabilities": ["dashboard.frame.v1", "dashboard.refresh.v1",
                   "dashboard.pair.v1", "config.v2",
                   "action.restart", "action.sleep", "power.hybrid.v1",
                   "voice.hub.v1"],
  "device": {
    "name": "Poulailler Terminal",
    "model": "zectrix-s3-epaper-4.2",
    "hardware": "NOTE4C 4-color",
    "panel": "400x300, 4-color BWRY",
    "fw": "Marvin 0.2",
    "upstream_base": "6.5.9"
  },
  "config_revision": 7
}
```

`upstream_base` is the vendor firmware version this project was forked from. It
is reported because it is a true fact about provenance, and it is labelled
because it is not a version of this firmware. `firmware` and `device.fw` are
the same string and both move when this firmware moves.

Everything else in the status response is unchanged from api 1, except that a
build advertising `power.hybrid.v1` adds a `power` object:

```json
"power": {
  "contract": 1,
  "mode": "auto_saver", "desired_mode": "auto_saver", "ack": "acknowledged",
  "awake": true, "sleep_intent": true,
  "interactive_remaining_s": 0,
  "wake_interval_min": 60,
  "timer_armed": false, "next_wake_in_s": 0, "next_wake_epoch": null,
  "last_wake_reason": "timer", "last_outcome": "updated",
  "budget_exhausted_phase": null,
  "consecutive_failures": 0,
  "battery": {"present": true, "calibrated": true, "plausible": true,
              "mv": 3912, "percent": 57},
  "charge": {"state": "no_power", "charging": false}
}
```

Three of these fields are nullable on purpose, and the reason is the same in
each case: the honest answer is sometimes "this device cannot know".

- **`battery.mv` and `battery.percent` are null unless the reading is fit to
  show.** The board reads VBAT through a divider on ADC1 channel 3 and converts
  with a curve-fitting calibration scheme that only exists if this particular
  chip was factory calibrated. When it is absent the driver has counts and no
  volts, and a percentage derived from uncalibrated counts is a number with a
  percent sign after it. `calibrated` and `plausible` say which gate failed.
- **`next_wake_epoch` is null until the clock has been set.** A wall-clock time
  computed from a clock that starts at 1970 is a fabricated timestamp, and a
  client would render it as a real one. `next_wake_in_s` is always a countdown
  and does not depend on the clock.
- **`mode` and `desired_mode` are separate, and `ack` says whether they agree.**
  A device can be asked for a mode it has not taken up yet, and flattening the
  two would let a client claim a device had obeyed a command.
- **`last_outcome` is null until a wake cycle has actually finished.** A device
  that has just booted has not completed one. Naming an outcome anyway would
  tell a client the last update succeeded on a device that has never updated,
  which is the reassuring-and-false answer this whole object exists to avoid.
- **`budget_exhausted_phase` is null unless the wake budget ran out**, and
  names the phase it ran out in (`network`, `fetch`, `render`, `settle`) when
  it did. "We gave up waiting for DHCP" is a far more useful field report than
  "we gave up".

`mode` is the *effective* mode right now, including an interactive window that
is currently open. `GET /api/v1/config` reports the *base* mode instead: see
there for why.

### `sync.sync_interval` no longer governs sleep

`power.wake_interval_min` is how long the device sleeps before it wakes.
`sync.sync_interval` used to be how long it stayed awake before sleeping, and
in `auto_saver` it is **no longer consulted at all**.

That is a deliberate change rather than an oversight. It was the only thing
that ever put this device to sleep, which had two consequences: a timer wake
stayed awake for the whole `sync.sync_interval` (thirty minutes by default)
instead of the two minutes the wake cycle needs, and `sync.sync_interval = 0`
meant "never sleep" — so one legacy key at zero silently disabled power saving
on a device whose config, status route and UI all reported it was saving power.

In `auto_saver` the awake period is now bounded by the wake budget (120 s
total; see `main/common/power_policy.h`), and the schedule comes from
`power.wake_interval_min`, which has a floor of 15 minutes and cannot express
"never". `sync.sync_interval` remains writable and is still reported, but it
does not decide whether an `auto_saver` device sleeps.

## `GET /api/v1/config`

Requires `X-Auth-Token`.

```json
{
  "api": 2,
  "revision": 7,
  "config": {
    "gallery":   {"slide_min": 5},
    "sync":      {"sync_interval": 30},
    "voice":     {"muted": true, "hub_url": "http://<panel-ip>:8770",
                  "hub_token_set": true},
    "dashboard": {"lockdown": true},
    "network":   {"lan_service": true, "wifi_writable": false},
    "power":     {"mode": "auto_saver", "interactive_min": 15,
                  "wake_interval_min": 60}
  }
}
```

`power.mode` here is the **base** mode, which is what survives a reboot. An
open interactive window is live state with a deadline and is reported on the
status route next to the countdown that makes it meaningful; it is never in
this response, because a window that came back from NVS would be a window
nobody opened.

`hub_token_set` is a boolean and there is no field in this response that could
hold the token. The type the firmware renders from does not carry one, so this
is a property of the design rather than of the handler; `test_device_config.cc`
searches the rendered bytes for the secret to keep it that way.

`wifi_writable` is stated rather than left out, so a client cannot mistake the
absence of Wi-Fi fields for an oversight it should work around. Credentials are
set through the physical AP portal and nowhere else.

## The revision

`revision` is a monotonic counter bumped by **every** settings change, whether
it arrived over the network or from somebody pressing buttons on the device. It
is persisted, so it does not restart at zero after a reboot and match a stale
read by accident.

Zero means nothing has ever changed. The counter skips back to 1 rather than 0
on wrap, so that stays true.

## `PATCH /api/v1/config`

Requires `X-Auth-Token`.

```json
{
  "expected_revision": 7,
  "set": {"gallery.slide_min": 30, "voice.muted": false},
  "confirm": "lan_service_off"
}
```

`expected_revision` is required. A patch whose revision does not match the
device's current one is refused with `409` and the current revision, so the
loser of a race can re-read and reconcile in one round trip instead of two.
This is what stops the tower from overwriting a change made by hand thirty
seconds earlier.

`set` accepts only these dotted names. There is no generic setter and no
pattern match: a name not in this table is a `400 unknown_field` carrying the
name, not a key written somewhere.

| Field | Type | Accepts | Applied |
|---|---|---|---|
| `gallery.slide_min` | integer | 0, 5, 10, 30 | `immediate` |
| `sync.sync_interval` | integer | 0 to 1440 minutes | `immediate` |
| `voice.muted` | boolean | strict true or false | `immediate` |
| `voice.hub_url` | string | http or https, or empty to clear | `immediate` |
| `dashboard.lockdown` | boolean | **true only** | `immediate` |
| `network.lan_service` | boolean | true, or false with `confirm` | `immediate_not_persisted` |
| `power.mode` | string | `auto_saver`, `interactive`, `always_on` | `immediate` |
| `power.interactive_min` | integer | 5, 15, 30, 60 | `immediate` |
| `power.wake_interval_min` | integer | 15 to 1440 minutes | `immediate` |
| `autonomy.enabled` | boolean | strict true or false | `immediate` |

Types are strict. `1` is not `true`, and `5.5` is not a slideshow interval.
Accepting either would mean a client bug silently unmuting a microphone or
applying a value nobody sent.

### The three apply modes

- `immediate` takes effect now and survives a reboot.
- `immediate_not_persisted` takes effect now and is gone at the next boot.
  `network.lan_service` is runtime-only state in `application.cc`: nothing
  writes the choice to NVS and the server restarts with Wi-Fi. Calling that
  "immediate" would let a client claim the setting had stuck.
- `restart_required` is stored now and in force after a restart. No field uses
  it today. It exists because the honest answer for a future field may be that
  one, and a contract that cannot express it would force a lie.

#### `autonomy.enabled` changed mode, and why

It was `immediate_not_persisted` while a local render could only happen because
somebody asked for one over the API. Nothing behind the field wrote to NVS, so
it held for the life of the boot and a power cycle returned the device to off —
a bench enable did not outlive the bench, and the honest mode said so.

It is `immediate` now, and the reason is not that the contract got weaker. The
autonomous wake cycle reaches the panel by going *through deep sleep*, and a
deep sleep is a reboot. An unpersisted switch would have come back off on the
very first wake, so the feature could never have run unattended — which is the
only thing it is for. Every wake would have needed an operator to re-enable it,
and the device would have looked broken rather than switched off.

The field is now persisted (NVS namespace `autonomy`, key `enabled`), so
`immediate` is the true answer. The default when the key has never been written
is still `false`: persistence remembers a decision, it does not make one. A
device that has never been asked is a push target, exactly as before.

Turning it off is still the safe direction and still takes effect at once. The
stored profile is kept either way, so turning it back on does not require a
re-push.

### Refusals

All or nothing. A patch that fails on any field applies none of them and does
not move the revision, so a caller that re-reads sees exactly what it saw
before it tried.

| Error | Status | Meaning |
|---|---|---|
| `revision_mismatch` | 409 | somebody changed the configuration first; body carries `revision` |
| `lockdown_is_one_way` | 403 | lockdown may be raised remotely and never lowered |
| `unknown_field` | 400 | the name is not in the table above; body carries `field` |
| `wrong_type` | 400 | right name, wrong JSON type |
| `out_of_range` | 400 | right type, outside the bounds the device menu itself enforces |
| `missing_expected_revision` | 400 | no compare-and-swap token was sent |
| `missing_confirmation` | 400 | a field that needs a literal did not get one |
| `bad_confirmation` | 400 | the literal did not match |
| `hub_url_needs_token` | 400 | a URL with no token behind it configures nothing |
| `bad_hub_url` | 400 | failed the same validation the v1 hub route applies |
| `duplicate_field`, `empty_patch`, `not_object`, `no_set_object`, `value_too_long` | 400 | malformed body |

### Lockdown is one way

`dashboard.lockdown` can be set to `true` from the network and never to
`false`, in either current state. Raising the drawbridge is safe from anywhere;
lowering it re-opens the unauthenticated legacy gallery write routes, and that
is a decision for somebody holding the device, not for whoever currently holds
the token. The device's own Settings menu can still turn it off.

### Turning the LAN service off

`network.lan_service: false` requires `"confirm": "lan_service_off"` in the
body. It severs the API the caller is talking through. Only a button press on
the device, or the next Wi-Fi reconnection, brings it back.

Turning it **on** needs no literal: switching the API back on cannot strand
anybody. Note that starting the server needs Wi-Fi and an address, so the
response reports the state the device is actually in, which is not always the
one that was asked for.

### The hub URL and its token

`PATCH` can change `voice.hub_url` and can never send a token: there is no
field for one. Install the pair with `POST /api/v1/voice/hub`, which is
unchanged from api 1 except that it now also reports `token_set` and the new
`revision`. A `PATCH` that sets a non-empty URL on a device with no stored
token is refused with `hub_url_needs_token`, because a URL alone would leave
the device claiming to be configured.

Setting `voice.hub_url` to the empty string clears the pair, token included. A
token with nowhere to go is a stored secret with no purpose.

## `POST /api/v1/actions/restart` and `POST /api/v1/actions/sleep`

Requires `X-Auth-Token`, an `Idempotency-Key` header, and a body:

```json
{"confirm": "restart"}
```

The literal is the action's own name, so a body that reaches the wrong route
cannot confirm the action it arrived at.

The `Idempotency-Key` is required, not optional politeness. Without one there
is no way to tell a retry from a second deliberate reboot, and the safe reading
of that ambiguity is to refuse rather than to guess. A repeat of a key already
served is answered `200` with `"replay": true` and schedules nothing.

```json
{"action": "restart", "scheduled": true, "replay": false, "at_ms": 1000}
```

`202` when it was scheduled, `200` on a replay. The device answers first and
acts a second later, because the response has to reach the socket before the
device stops being a device.

`sleep` stops the LAN service and Wi-Fi, then enters deep sleep.

**This changed with `power.hybrid.v1`, and the old sentence here was "only the
physical BOOT button wakes it".** That is still true of the network — no client
can bring the device back — but it is no longer the whole truth, because the
device now also arms its own wake timer before sleeping. `sleep` ends the
current interactive window; it does not switch off the automatic hourly
refresh, which would be a surprising thing for a button labelled "sleep" to do
on a product whose promise is an automatic hourly update.

So, precisely:

- The BOOT button wakes it immediately. This is the only immediate wake there is.
- Its own timer wakes it after `power.wake_interval_min`, whereupon it fetches,
  refreshes if the frame changed, and sleeps again.
- **Nothing on the network can wake it.** Deep sleep powers the Wi-Fi radio
  down; there is no socket listening. A client that wants a sleeping device in
  a different mode has to hold that request and apply it at the next wake,
  which is what the tower does.

### The legacy `/settings` page sleeps the same way now

The device also carries the pre-v2 transfer page, and its "Stop and save power"
button posts `{"sleep": true}` to the legacy `/settings` route. That path used
to call `esp_deep_sleep_start()` having armed **ext0 and nothing else**, so a
device put to sleep from the gallery page never woke on its own again: the
hourly refresh simply stopped until somebody found it and pressed BOOT. The
device advertised `power.hybrid.v1` and one of its own buttons quietly opted
out of it.

It no longer has its own opinion about sleep. The legacy handler hands the
request to the same `Action::kSleep` this route drives, so both end in
`Application::EnterManualSleep()`: the base power mode is restored, the current
interactive window is closed, and **both** wake sources are armed from the
configured `power.wake_interval_min`. There is deliberately no second default
interval defined anywhere near that page.

The delegation is logged (`Legacy web sleep delegated to the device sleep
action`). If the config service has no action runner wired up — which on a
booted device means something is badly wrong — the old button-only sleep
remains as a last resort and says so at `ERROR` level rather than logging a
normal sleep.

## What is deliberately absent

These are not missing. They were considered and left out:

- **Wi-Fi credential write.** The physical AP portal stays the boundary.
- **Pairing initiation.** Pairing still starts with a button press, inside a
  window that nothing on the network can open.
- **OTA.** No route on this device writes an image to it.
- **Gallery mutation.** The frame slot is separate storage; the gallery cannot
  grow through this API by construction.
- **Raw NVS.** `Field` is a closed enum and the dotted names are a fixed table.
  A config API whose field set is open is a raw NVS API wearing a hat.
- **Any route that returns a secret.** The token is a boolean everywhere it
  appears.
- **A remote wake.** Considered, and impossible rather than merely declined: a
  device in deep sleep has no radio powered and nothing listening on any
  socket. There is no route that could receive the request. The tower's answer
  is to hold the intent and apply it at the next wake, and to say so plainly
  rather than offering a button that cannot work.

## Route budget

The AP transfer server registers 11 legacy routes, 6 dashboard v1 routes and 4
config v2 routes, against `max_uri_handlers = 26`. `httpd` refuses a
registration past that limit and the v2 routes register last, so a budget that
was too small would take the config API out quietly rather than loudly. The
count is in `ap_transfer_server.cc` next to the number.
