# Compatibility

What this software has actually been run against, stated as narrowly as the
evidence allows.

## Devices

**NOTE4C only.** This is not a general e-paper dashboard tool and does not aim
to become one. The renderer packs 400×300 pixels at 2 bits each into exactly
30000 bytes in one specific four-colour palette, the HTTP client speaks one
specific local API, and the settings page is generated from a registry of that
firmware's fields. None of that is abstracted behind a driver interface,
because a driver interface with one implementation is a guess about the second
one.

No other panel is targeted, tested, or known to work. If you have a different
device, this repository is more useful as a worked example than as a program.

| | |
| --- | --- |
| Panel | 400×300, four colours: black, white, yellow, red |
| Frame format | 2 bits per pixel, MSB first, exactly 30000 bytes |
| Transport | HTTP on the LAN, port 80, private IPv4 literal only |
| Auth | a device token in the `X-Auth-Token` header |

## Firmware API levels

The tower negotiates at runtime: it reads the device's `api` number and its
capability list, and renders only what the device says it can do.

| Level | What the tower offers |
| --- | --- |
| **api 1** | Read status, push frames, read the ledger. Every setting renders as **on-device only**, with the reason on the row. No configuration is written. |
| **api 2** with `config.v2` | Typed settings become live controls, written in one batch with a compare-and-swap on the device's config revision. Settings that do not survive a restart say so on their own row. |
| **api 2** with `power.hybrid.v1` | The power panel appears: mode, interactive windows, next wake, battery when the device can honestly measure it, and the pending-intent machinery for a device that is asleep. |
| **api 2** with `voice.ptt.v1` | The Voice page reports the capture path as compiled in. It still says, in as many words, that "compiled in" is not "validated on hardware". |

A capability the device does not advertise produces **no control at all**,
never a control that would be refused. The distinction is enforced structurally
by `src/core/registry.ts`: an entry with `transport: "none"` cannot render an
editable control.

## What has actually been exercised

Be suspicious of anything not in this table.

| Path | Evidence |
| --- | --- |
| In-repo mock, api 2, all capabilities | The whole unit suite and the whole browser suite, on every release of this repository. This is the path that is genuinely well tested. |
| Real panel, push to `verified_displayed` | Executed on one physical device. The push ledger holds verified entries with a measured panel refresh of about 25 s. |
| Real panel, api 1 settings | Observed: every row renders as on-device only, as designed. |
| Real panel, api 2 settings | **Not executed on hardware** beyond the power fields below. The rest of the settings path is covered against the mock only. |
| Real panel, `power.hybrid.v1` | **Executed on hardware.** A `power.hybrid.v1` build was flashed to the same physical NOTE4C. See the cycle below for exactly what was observed, and what was not. |
| Voice capture | **Never validated on hardware.** No audio path on this board has been confirmed to work by anybody involved in this project. Nothing in the power cycle below bears on it: the two features share a device and nothing else. |

### The one hybrid power cycle that was observed

Reported as a list of observations rather than as a conclusion, because it is
one cycle on one device and the interesting claims are the ones it does *not*
support.

| Observed | What was seen |
| --- | --- |
| Firmware | A `power.hybrid.v1` build, flashed to the same physical NOTE4C every other real-device claim on this page refers to. |
| Deep sleep | Entered. The device stopped answering, which is what deep sleep looks like from the tower: the radio is off and there is no socket. |
| Button wake | Confirmed. Pressing the button on the device brought it back and it answered again. |
| Timer wake | Confirmed. With the wake interval set to 15 minutes, the device woke on its own timer after **911 s** — about eleven seconds past the nominal 900. |
| Wake interval, re-read | **60 min**, read back from the device at the end of the cycle. That is the value it was left on; it is not the interval the 911 s wake was measured under. |
| Battery | **97%**, as the device reported it on that cycle. |

**What this cycle does not establish, and is not offered as.**

- **It is not a battery-life measurement.** A single 97% reading is one sample
  from one charge state at one moment. It says the device had a reading it
  considered plausible; it says nothing about how long the device runs, and
  nothing on this page should be read as an autonomy figure. There is no
  autonomy figure.
- **It is not a validation of the voice path.** No audio was captured, played
  or confirmed. That row above still says never validated, and this cycle does
  not change it.
- **It is one device, one build, one cycle.** The 911 s is a measurement, not a
  specification: it was not repeated, and nothing here establishes how the
  timer behaves across temperatures, charge states or intervals other than the
  one it was set to.

## Host requirements

| | |
| --- | --- |
| Node | 20 or newer |
| Browsers | Current Chrome, Safari and Firefox. The interface is plain React with no browser-specific API beyond `matchMedia`, `ResizeObserver` and canvas. |
| OS | Developed on macOS. The tower itself is portable Node; two *optional sources* are macOS-specific — the Apple Reminders adapter shells out to `remindctl`, and the calendar source reads a snapshot file written by an EventKit worker that is not part of this repository. Both are off unless configured, and everything else runs anywhere Node does. |
| Python | Only for the maintainer tools in `tools/` (atlas generation, rasteriser measurements). Not needed to run or develop the tower. |

## Viewports

The browser suite runs three, and the interface is designed for all three
rather than scaled down to them:

| | |
| --- | --- |
| 1440×900 | Desktop: left rail, two columns, designer sidebar |
| 768×1024 | Tablet portrait: left rail, single column, designer sheet |
| 375×812 | Phone: bottom tab bar, the rail reflowed into a utility strip, single column, fit-to-width canvas, sticky actions |

The tests assert that no page scrolls sideways at 375 px, that every touch
target measures at least 44 px in its smaller dimension, and that every shell
affordance — Sign out, the density toggle, the device state badge, the quick
interactive request — exists exactly once and is reachable at all three
widths.
