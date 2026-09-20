# NOTE4C instruction manual

What the three buttons do, what each screen is for, and what the device does
when nothing is working. Every claim below carries a `file:line` reference to
the code it describes. Where the code does not support a claim, the claim is
not made.

Status of this document: derived from source at the Milestone A commit. Nothing
in it has been verified on the physical device. Gesture behaviour is host-tested
(`tests/host/test_nav_model.cc`, `tests/host/test_button_gestures.cc`) and the
firmware compiles clean, but no button on the real board has been pressed by
anyone since this navigation model was written. The hardware checks that would
change that are listed in `docs/HARDWARE-ACCEPTANCE.md`.

The gesture tables below come from `tests/nav-map.json`, which is also what the
host tests assert against. `tests/test_manual.py` fails if a row in that file is
missing here, so the manual cannot quietly drift from the firmware.

---

## 1. The three buttons

| Button | GPIO | Notes |
| --- | --- | --- |
| UP | GPIO39 | `main/boards/zectrix-s3-epaper-4.2/config.h:23` |
| DOWN | GPIO18 | Shared with the battery power rail, `config.h:24-26` |
| BOOT | GPIO0 | Also the boot strap pin, `config.h:20,27` |

A press counts as long at 1000 ms (`zectrix-s3-epaper-4.2.cc:36`). A press
shorter than that is a click. The click that the driver emits when you let go
of a long press is swallowed, so one hold produces one action and not two
(`main/common/button_gestures.cc:70-100`).

DOWN sharing a line with the battery power rail is flagged in the board's own
header as a multiplex (`config.h:25-26`). Holding DOWN is an upstream gesture
that this firmware keeps, but the interaction between a long hold and the power
rail has never been checked on hardware. It is check HG4 in
`docs/HARDWARE-ACCEPTANCE.md`.

## 2. The pages, and how to move between them

Three pages form a ring, and the ring wraps
(`main/common/nav_model.cc:17-21`):

```
Dashboard  <->  Gallery  <->  Settings  <->  Dashboard
```

On the Dashboard, UP goes to the previous page and DOWN to the next. On Gallery
and Settings, UP and DOWN move the selection within the page instead: with three
buttons a list needs them. That is the one place where UP and DOWN mean
something other than "previous and next", and it is why the two universal
gestures exist:

- **UP long press is Back.** It closes the innermost thing that is open, in this
  order: modal dialog, quick switch menu, fullscreen photo, photo transfer, then
  the page itself back to the Dashboard (`main/common/nav_model.cc:66-83`).
- **DOWN long press is Home.** It closes everything and shows the Dashboard,
  from any screen (`main/ui/rawdraw_ui_manager.cc`, `GoHome`).

Photo transfer is not on the ring. It is a mode you enter from Settings and
leave with Back or Home.

## 3. Every gesture on every screen

### Dashboard

The full-panel frame composed on the Mac. No overlay open.

| Gesture | What it does |
| --- | --- |
| UP click | Previous page in the ring |
| DOWN click | Next page in the ring |
| BOOT click | Open the quick switch menu |
| UP long press | Back, already at the top level |
| DOWN long press | Home, already home |
| BOOT long press | Reserved for push-to-talk |
| UP+DOWN long press | Open the Wi-Fi setup access point |

### Gallery

Photo grid (memory-card view), not the fullscreen photo view.

| Gesture | What it does |
| --- | --- |
| UP click | Move the selection up |
| DOWN click | Move the selection down |
| BOOT click | Open or close the fullscreen photo |
| UP long press | Back to the Dashboard |
| DOWN long press | Home to the Dashboard |
| BOOT long press | Reserved for push-to-talk |
| UP+DOWN long press | Open the Wi-Fi setup access point |

### Settings

The settings list.

| Gesture | What it does |
| --- | --- |
| UP click | Move the selection up |
| DOWN click | Move the selection down |
| BOOT click | Activate the selected setting |
| UP long press | Back to the Dashboard |
| DOWN long press | Home to the Dashboard |
| BOOT long press | Reserved for push-to-talk |
| UP+DOWN long press | Open the Wi-Fi setup access point |

### Photo transfer

Modal AP photo transfer, entered from the Settings row 'Photo transfer'.

| Gesture | What it does |
| --- | --- |
| UP click | Nothing to select on the instruction sheet |
| DOWN click | Nothing to select on the instruction sheet |
| BOOT click | Redraw the instructions from stored content |
| UP long press | Back, stopping transfer, to Settings |
| DOWN long press | Home, stopping transfer, to the Dashboard |
| BOOT long press | Reserved for push-to-talk |
| UP+DOWN long press | Open the Wi-Fi setup access point |

### Quick switch menu

The overlay opened by BOOT click on the Dashboard.

| Gesture | What it does |
| --- | --- |
| UP click | Move the menu selection up |
| DOWN click | Move the menu selection down |
| BOOT click | Activate the highlighted entry |
| UP long press | Back, closing the menu |
| DOWN long press | Home, closing the menu |
| BOOT long press | Reserved for push-to-talk |
| UP+DOWN long press | Open the Wi-Fi setup access point |

### Modal dialog

Reserved. No component in this firmware opens a modal dialog yet; the contract is fixed in advance.

| Gesture | What it does |
| --- | --- |
| UP click | Move the dialog selection up |
| DOWN click | Move the dialog selection down |
| BOOT click | Confirm the dialog |
| UP long press | Back, dismissing the dialog |
| DOWN long press | Home, dismissing the dialog |
| BOOT long press | Reserved for push-to-talk |
| UP+DOWN long press | Open the Wi-Fi setup access point |

The modal dialog rows describe a contract, not a screen you can reach. The only
dialog in the firmware is the gallery's delete confirmation, and it opens on a
BOOT double click (`main/ui/renderers/rawdraw/photo_gallery.cc:240-247`) which
the board never delivers, because `OnDoubleClick` is not registered
(`zectrix-s3-epaper-4.2.cc`, `InitializeButtons`). The rows exist so that Back
and Home already have the right meaning if a dialog is ever wired up.

## 4. The quick switch menu

BOOT click on the Dashboard opens a small menu
(`main/ui/rawdraw_ui_manager.cc:706-720`):

| Entry | What it does |
| --- | --- |
| Dashboard | Go to the Dashboard |
| Gallery | Go to the Gallery |
| Settings | Go to Settings |
| Repaint screen | Redraw the frame already stored on the device |

**"Repaint screen" is not an update.** It redraws what the device already has.
It does not ask the Mac for anything, because the device has no route to the
Mac: the composer listens on loopback and on the tailnet, and the device sits on
the ordinary LAN (`main/application.cc:487-493`). A repaint is useful after
ghosting or a failed refresh, and it is the only thing it claims to be.

Frames arrive by push, from the Mac, when the Mac decides to send one.

## 5. Settings

The list, in order (`main/application.cc:224-362`):

| Row | What it is |
| --- | --- |
| Restart | Reboots the device |
| Slideshow interval | Off, 5, 10 or 30 minutes for the gallery slideshow |
| Wi-Fi | Turns the station connection on or off |
| LAN service | The local HTTP service the Mac pushes frames to |
| LAN address | The device's address, once it has one |
| Power saving | Enter deep sleep now |
| Firmware | Version |
| Pair dashboard | Opens a pairing window so the Mac can install a token |
| Block legacy writes | Refuses the older unauthenticated photo routes |
| Photo transfer | Starts the photo transfer access point |
| Mute voice (software) | Suppresses recording and playback, and is remembered across a power cut |

"Pair dashboard" opens a time-limited window; the token itself is never drawn on
screen. E-paper keeps its last image with the power off, so anything shown there
is effectively left lying around (`main/application.cc:322-340`, and
`docs/PROVISIONING.md`). The device offers no way to pair over the network: you
have to be holding it.

"Photo transfer" and "Mute voice (software)" are appended at the end of the list
on purpose. Two settings rows are addressed by hard-coded index
(`main/application.cc`, `kSettingsPairIndex` and `kSettingsLockdownIndex`), so
new rows can only be added at the end without breaking them. The mute row reads
its own index from the list as it is built rather than adding a third constant
to keep in step.

"Mute voice (software)" is described in section 9.

## 6. What the Dashboard shows when there is no picture

The Dashboard is a full-panel image composed on the Mac. Until one arrives, it
draws a placeholder that says what is actually wrong, in this order
(`main/common/nav_model.cc`, `StatusLine`):

| Situation | What it says |
| --- | --- |
| Photo transfer running | Photo transfer is running. |
| No Wi-Fi | Waiting for Wi-Fi. |
| Wi-Fi, not paired | Not paired yet. |
| Paired, no frame yet | Paired. Waiting for the first frame. |
| Frame stored, LAN service off | LAN service is off, so no new frame can arrive. |
| Frame stored, LAN service on | LAN service is on and ready to receive. |

None of these ever claims the device is fetching or updating, and a host test
asserts that across every combination of inputs
(`tests/host/test_nav_model.cc`, `test_status_never_claims_a_refresh_from_source`).

## 7. When the screen is busy

A four-color refresh is slow. The driver waits up to 120 seconds for the panel
three times per refresh, so one refresh can legitimately occupy the device for
several minutes (`main/application.cc:34-47`).

A button press during a refresh is **not** lost. The most recent navigation
click is held and applied when the panel finishes
(`main/common/nav_model.cc`, `Decide`, and `PumpPendingNavIntent` in
`main/ui/rawdraw_ui_manager.cc`). The activity LED flashes immediately, so the
button is visibly alive even when the screen will not change for another minute.

Only the most recent click is held. Pressing DOWN five times during a refresh
moves one page, not five. Back and Home discard a held click, since they are
immediate and a stale page change afterwards would undo them.

## 8. Sleep and waking

The device sleeps after the sync interval, 30 minutes by default, unless the LAN
service or the gallery slideshow is running (`main/application.cc:620-680`).

Only BOOT wakes it (`esp_sleep_enable_ext0_wakeup` on `BOOT_BUTTON_GPIO`,
`main/application.cc`). UP and DOWN do nothing on a sleeping device. The first
BOOT press after a sleep wakes the device; it is not also delivered as a
gesture.

"Power saving" in Settings sleeps immediately, and stops the LAN service first
so it is not killed mid-transfer.

## 9. Push-to-talk: compiled in, muted, and never heard

Read this section before turning the mute off.

**The microphone path is compiled into this firmware** (`VOICE_PTT_ENABLED=1`),
and **none of it has been tested on hardware**. Not the speaker, not the
microphone, not the amplifier. Whether this board makes any sound at all, and
whether its microphone captures anything, are `HG5.1` and `HG5.4` in
`docs/HARDWARE-ACCEPTANCE.md` and both are open. Turning the mute off on a
physical device *is* that test.

Two things stop anything from being recorded on a device that has just been
flashed:

1. **The software mute defaults to on.** A device with no value stored under
   NVS `voice`/`muted` comes up muted. Turning it off in Settings is a
   deliberate act.
2. **No hub is configured.** Without a hub address and token the device has
   nowhere to send an utterance, and it refuses to record rather than filling
   fifteen seconds it would then throw away.

### How it works when both of those are dealt with

| You do | What happens |
| --- | --- |
| Press BOOT and hold | A short tone acknowledges the press immediately |
| Keep holding past about a third of a second | A rising two-note tone, and the microphone opens |
| Speak, then let go | A falling two-note tone; the recording is sent |
| Let go before the rising tone | Nothing was recorded. The press counts as a BOOT click |
| Hold UP for Back while it is waiting | The request is abandoned and the device goes quiet |

The tones matter more than they sound like they should. A four-colour refresh
on this panel takes tens of seconds, so the screen cannot tell you anything
while your finger is on the button. The tone and the LED are the only feedback
that arrives in time, and neither of them waits for the panel.

A press that recorded does **not** also do what a BOOT click does. On the
Dashboard a BOOT click opens the quick switch; a hold that reached the
microphone swallows that click, so one press never produces two outcomes.

### Limits, all of them deliberate

| Limit | Value |
| --- | --- |
| Shortest utterance that is sent | 0.4 s. Below that it is discarded and a low blip says so |
| Longest utterance | 15 s. The recording stops itself and sends what it has |
| Longest body | 192 KiB. A longer recording is truncated, sent anyway, and logged as truncated |
| How long the device waits for an answer | 25 s |
| Upload attempts | 3, six seconds each, one second apart |

### Why the device does not answer out loud

It cannot. There is no speech synthesis in this firmware, and the hub's v1 wire
returns text and no audio (`hub/CONTRACTS.md` section 10). When an answer
arrives you get the three-note response tone and a log line saying how long the
reply was. The device does not read it to you and does not claim to.

The device also never conflates *received*, *transcribed* and *executed*. If the
hub is running with its default stub adapters, nothing was transcribed and
nothing was asked; the response says `"source": "stub"` and the device logs a
warning saying exactly that.

### The gesture belongs to push-to-talk

Since a BOOT hold is push-to-talk, no screen may tell you to hold BOOT to leave,
to cancel, or to open anything.
Three modal screens used to say "Hold BOOT to exit" before push-to-talk took the
gesture over, and no longer do. Leaving any screen
is UP long press for Back or DOWN long press for Home, everywhere.
`tests/test_manual.py` fails the build if a renderer or this manual reintroduces
a BOOT hold that is not push-to-talk.

### Mute

The Settings row **Mute voice (software)** turns the voice path off and the
setting survives a power cut: it is written to NVS (namespace `voice`, key
`muted`) and restored at boot before any gesture can reach the state machine
(`main/application.cc`, `InitializeAudioFsm`). It is a *software* mute, which is
why the row says so. It suppresses recording and it suppresses spoken responses;
it does not cut power to the microphone or the amplifier, and nothing in this
device can promise that it does.

The mute does not silence the short interface tones. Those are feedback about a
button, and they are how a muted device tells you it is muted.

A muted device stays silent about it until you actually hold the button. A quick
BOOT click makes no complaint, because on the Dashboard a BOOT click is the
quick switch and a blip on every one of them would be unusable.

### Configuring the hub

There is no way to type a URL on three buttons, so the address and token are
installed over the network with a single authenticated request:

```
POST /api/v1/voice/hub
X-Auth-Token: <the dashboard token from pairing>
{"url": "http://<panel-ip>:8653", "token": "<the hub token>"}
```

Sending `{"url": "", "token": ""}` clears it. The response never echoes the hub
token back and the device never logs it. Configuring a hub does not turn
anything on: the mute is separate and is still whatever you left it.

The hub itself binds `localhost` by default and cannot be reached by the device
until somebody starts it with both `--bind` and `--allow-lan`. That is a
decision about the household network, not a default.

## 10. Offline behaviour

- **No Wi-Fi**: the stored dashboard frame keeps showing. E-paper holds its image
  without power, so the last frame stays on the panel indefinitely. The
  placeholder appears only if no frame was ever stored.
- **LAN service off**: nothing can push a new frame. Navigation, the gallery and
  settings all keep working; they are local.
- **Mac off**: the same as LAN service off, from the device's point of view. The
  device never polls, so it does not report the Mac as unreachable; it reports
  what it has.
- **Unpaired**: writes to the dashboard routes are refused, with no network
  enrollment path (`docs/PROVISIONING.md`). This is deliberate: a device that
  accepts a token from the network is a device whose screen belongs to whoever
  connects first.

## 11. Recovery

| Situation | What to do |
| --- | --- |
| Lost in a menu | Hold DOWN. Home works from every screen. |
| Screen looks wrong or ghosted | BOOT click, then "Repaint screen" |
| Wi-Fi credentials wrong | Hold UP and DOWN together to open the setup access point |
| Transfer mode stuck | Hold UP for Back, or DOWN for Home. Both stop the server cleanly. |
| Device unresponsive | Settings, Restart |
