# Pairing the dashboard

## Default posture

A device that has never been paired **refuses every dashboard write**, replying
`503 not_provisioned`. There is no default token, no compiled-in token, and no
token in this repository. This is not a warning to be configured away; it is the
shipped behaviour.

## How pairing works

1. On the device: **Settings → Pair dashboard**.
   The device mints a 32-byte token from the hardware RNG and opens a 120-second
   window. The token that is currently in use keeps working until the new one is
   claimed, so pressing this by accident does not orphan the bridge.

2. On the Mac, within those 120 seconds:

   ```sh
   python3 bridge/note4c_bridge.py --device <panel-ip> --pair
   ```

   The bridge claims the token and writes it to
   `secrets/device-token` with mode `0600`. It is not printed.

3. The window closes on the first successful claim. A second caller gets
   `403 not_pairing`.

Re-pairing at any time invalidates the previous token.

## Why this shape

The constraint was that pairing must require trusted local access, and must
never be unauthenticated remote enrollment or a committed secret. Three options
were considered:

**Show the token on the panel.** Rejected. E-paper keeps its last image with the
power off, so a token drawn on the screen stays legible on a device sitting on a
shelf indefinitely. That is worse than the problem it solves.

**Serial console over USB.** The strongest option, and the one to move to if the
residual risk below ever matters. Rejected for now because it requires a cable
for every re-pair, and because none of it could be exercised without the
hardware.

**Physically opened, single-claim, time-boxed window.** Chosen. Opening it
requires someone holding the device. Nothing reachable over the network opens
it. It self-closes on claim and on timeout.

### Residual risk, stated rather than buried

During the 120-second window, another host on the same LAN could reach the
device first and claim the token. Two things bound this:

- The window only exists while someone is standing at the device having just
  pressed the button, and it is 120 seconds long, not indefinite.
- Because the window is single-claim, a theft is *visible*: the operator's own
  `--pair` call fails with `403`. The correct response is to press
  **Pair dashboard** again immediately, which invalidates the stolen token
  before it can be used to write anything.

This trades a small, detectable window against requiring a USB cable. On a home
LAN behind a router that is the right trade; on a hostile network it is not, and
the serial route should be implemented instead.

## Token handling rules

These hold throughout the implementation:

- The token is never written to a log line, at any log level. `dashboard_manager.cc`
  logs `"pairing claimed; dashboard token installed"` and nothing more.
- It is never drawn on the panel.
- It is stored in NVS on the device and in a `0600` file on the Mac. The bridge
  refuses to read a token file that is group- or world-readable, rather than
  reading it and warning.
- The file is created with `0600` from the outset, not chmod-ed afterwards, so
  it is never briefly world-readable.
- Comparison is constant-time and happens on decoded bytes, so neither the value
  nor the number of matching leading characters leaks by timing.
- Nothing in this repository contains a real token. The fixed value in
  `tests/host/test_dashboard_service.cc` is `00112233…` — obviously synthetic,
  and only ever compared against itself.

## Turning it off

**Settings → Pair dashboard** on an already-paired device re-pairs it. To
withdraw access entirely, clear the token (`DashboardManager::ClearToken`), after
which the device returns to refusing all writes.
