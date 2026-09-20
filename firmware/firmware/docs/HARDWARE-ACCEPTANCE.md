# Hardware acceptance sheet

Nothing in this firmware has been hardware-tested. Not the navigation, not the
buttons, not a single audio path. This document is the list of checks that would
change that, one row per check, each with the evidence that would count as
having done it.

None of these has been executed. No row below may be marked done from a build
log, a host test or a code reading: the entire point of the list is that those
are the things that have already been done.

Every check that touches the device needs Alex's approval first. HG2 and HG3
additionally gate every flash that will ever happen to this device.

## Status vocabulary

Used here and in every report from this project:

| Term | Means |
| --- | --- |
| built | The code exists |
| host-tested | A host suite exercises the real translation unit and passes |
| compiled | `idf.py build` is clean for the shipped configuration |
| hardware-tested | It ran on the physical device and produced evidence |

Milestone A produced built, host-tested and compiled work. It produced nothing
hardware-tested, and no claim to the contrary is permitted.

The push-to-talk milestone that followed it is the same: built, host-tested and
compiled, on a board whose speaker and microphone have still never been heard.
The microphone path is now compiled into the shipped configuration, which is a
decision about what the first flash contains and is explained in HG5 below. It
changes nothing about what has been tested, which is none of it.

---

## HG1 Serial observation (read-only)

Attach serial, capture a boot log, change nothing.

| # | Check | Expected evidence |
| --- | --- | --- |
| HG1.1 | Device boots to the Dashboard | Boot log with `Switching page` reaching Dashboard |
| HG1.2 | ES8311 answers on I2C | Probe result in the log, address as configured in `config.h:6-18` |
| HG1.3 | SPIFFS mounts | `Photo storage ready (N photos)` from `application.cc` |
| HG1.4 | Dashboard store initialises | `Dashboard manager init failed` **absent** |
| HG1.5 | Heap and PSRAM figures | Free internal heap and PSRAM at boot, recorded as numbers |
| HG1.6 | No panic loop over 10 minutes | Uninterrupted log, no reset reason other than the first power-up |

Blocking for: everything else on this sheet.

## HG2 Backup and layout truth

Before any write to flash, ever.

| # | Check | Expected evidence |
| --- | --- | --- |
| HG2.1 | Full 16 MB read back | `read_flash 0 0x1000000` image plus its SHA-256, stored beside the factory backup |
| HG2.2 | Backup is not truncated | File is exactly 16777216 bytes |
| HG2.3 | Partition table read from the device | `read_flash 0x8000 0xc00` decoded, compared field by field with `partitions/v2/16m.csv` |
| HG2.4 | otadata decoded | `read_flash 0xd000 0x2000`, which slot is active and its sequence number |
| HG2.5 | NVS is not empty on the device | Wi-Fi credentials present, so a merged-image flash at 0x0 is understood to destroy them |

Commands and reasoning: `REPORT.md` section 8. Backups live in
`/Users/marvin/projects/labs/note4c-setup/`, a folder this project otherwise
never reads.

Blocking for: HG3, and therefore every flash.

## HG3 Flash approval

| # | Check | Expected evidence |
| --- | --- | --- |
| HG3.1 | Parent user approves this specific artifact | Approval quoting the SHA-256 of the exact image |
| HG3.2 | HG2 is complete and its backup verified readable | Backup SHA re-computed from the stored file |
| HG3.3 | Rollback is one command | The command written out, with the backup path in it |
| HG3.4 | The merged image is **not** written at 0x0 | Flash arguments recorded; merged-at-0x0 erases NVS, proven in `REPORT.md` 8.2 |
| HG3.5 | Post-flash readback matches | SHA of the app region read back equals the SHA of what was sent |

## HG4 Buttons, physically

The navigation model is host-tested. Which physical key is which, and whether
holding them is safe, is not.

| # | Check | Expected evidence |
| --- | --- | --- |
| HG4.1 | Which key is UP and which is DOWN | Photo or note mapping the physical position of GPIO39 and GPIO18 |
| HG4.2 | A long hold on DOWN does not disturb the power rail | 10 holds of 3 s each with no reset; GPIO18 is multiplexed with VBAT (`config.h:24-26`) |
| HG4.3 | BOOT long press does not trigger the boot strap | 10 holds with no bootloader entry |
| HG4.4 | The click after a long press is swallowed | Log shows one `Gesture:` line per hold, never a long plus a click |
| HG4.5 | The UP+DOWN combo fires exactly once | One `ComboLong` per two-key hold, including when the keys are released one at a time |
| HG4.6 | The first press after deep sleep wakes and is not also a gesture | Log across a sleep and wake |
| HG4.7 | Every row of `tests/nav-map.json` behaves as documented | One log line per gesture per screen, compared against `docs/MANUAL.md` |

## HG5 Audio bring-up

Assume nothing here works. Nothing about it has ever been observed.

| # | Check | Expected evidence |
| --- | --- | --- |
| HG5.1 | The speaker produces sound at all | A generated earcon is audible; which one, and at what volume |
| HG5.2 | Amplifier click on power-up and power-down | Recording or observation at the start and end of a tone |
| HG5.3 | GPIO46 ownership resolved **in code** | Done: `Es8311AudioCodec` is the sole owner. `BoardPowerBsp` no longer configures the pin and `PowerAmpOn/Off` are gone (`main/boards/zectrix-s3-epaper-4.2/board_power_bsp.h`). The **polarity** on the physical board is still unverified and is HG5.3b |
| HG5.3b | The amplifier polarity is right | With output enabled the amplifier is on, not off. `pa_inverted` defaults to false and nobody has checked |
| HG5.4 | The microphone captures anything | A raw capture, with its amplitude range |
| HG5.5 | Microphone quality is good enough for speech | A capture a person can understand. Single mono mic, no AEC reference |
| HG5.6 | Earcon latency after a button press | Measured milliseconds from press to first sample out |
| HG5.7 | The six earcons are distinguishable by ear | A note per earcon, particularly listen-start against listen-stop |

Blocking for: any claim that push-to-talk works, and for turning the software
mute off on a device somebody depends on.

**Not** blocking for shipping the compiled path, and the reason is worth being
explicit about. The first flash ships `VOICE_PTT_ENABLED=1` with the software
mute defaulting to on and no hub configured, so a freshly flashed device cannot
open its microphone until two separate deliberate acts have happened. Turning
the mute off on the physical device is how HG5 gets tested at all. The
alternative, shipping the path compiled out, would mean a second flash to test
audio, and a second flash is a second opportunity to brick the device.

That is a decision, not a discovery. Whoever turns the mute off is running
HG5.1 and HG5.4 for the first time, and should expect them to fail.

## HG5F First-flash push-to-talk sequence

Run in this order on the device flashed with `VOICE_PTT_ENABLED=1`. Every step
assumes HG1, HG2 and HG3 are complete, because this is after a flash.

Stop at the first row that fails and record what happened. A failed row here is
information about the hardware, not a reason to change the firmware.

| # | Step | What to do | Expected evidence |
| --- | --- | --- | --- |
| HG5F.1 | Boot muted | Power on, read the boot log | `Voice software mute restored from NVS: muted` and `Push-to-talk ready: voice path compiled in, hub not configured, mute on` |
| HG5F.2 | A BOOT click still quick-switches | On the Dashboard, tap BOOT | The quick switch opens. Exactly one `Gesture: BootClick` in the log, and **no** muted blip |
| HG5F.3 | A muted hold says so | Hold BOOT for two seconds | `Earcon: muted`, state reaches `Muted`, `mic_start` never logged, no page change |
| HG5F.4 | The speaker works at all (**HG5.1**) | During HG5F.3, listen | Whether anything was audible, and how loud. If nothing: stop, and record HG5.1 as failed |
| HG5F.5 | The tones are distinguishable (**HG5.7**) | Compare the acknowledge blip against the muted blip | A note on whether they are telling apart by ear |
| HG5F.6 | No hub means refusal, not recording | Turn the mute off in Settings, hold BOOT for two seconds | `Earcon: error`, reason `no network or no hub configured`, state `Error`, `mic_start` never logged |
| HG5F.7 | Configure the hub | Start the hub with `--bind <mac-lan-ip> --allow-lan`, then `POST /api/v1/voice/hub` with the URL and token | `{"configured": true, ...}`; device logs `Voice hub configured` and `Voice transport available` |
| HG5F.8 | Arm latency | Hold BOOT, time the gap to the rising two-note tone | Measured milliseconds. Expected about 300 ms; more than 400 ms means the pump or the button driver is slower than assumed |
| HG5F.9 | The microphone captures anything (**HG5.4**) | Hold BOOT for three seconds saying something, release | `Capture finished: N frames, M ms, K bytes` with N greater than zero. `N=0` means the codec input produced nothing: HG5.4 failed |
| HG5F.10 | The upload reaches the hub | Watch the hub log | One `utterance <id>: N audio bytes accepted` line. The device logs status 200 |
| HG5F.11 | The answer is labelled as a stub | Read the device log | `The hub answered with a stub adapter: nothing was transcribed and nothing was asked`. If this line is absent while the hub is running stubs, the labelling is broken and that is a bug |
| HG5F.12 | A short press does not upload | Hold BOOT for roughly half a second and release | Either a BOOT click, or reason `utterance below the minimum length` with no upload. Never both a click and an upload |
| HG5F.13 | A hold that recorded does not also click | On the Dashboard, hold BOOT for two seconds and release | The quick switch does **not** open |
| HG5F.14 | Back cancels a wait | Hold BOOT, release, then hold UP before the answer | State returns to Idle, `Utterance <id>: answer discarded, the user cancelled` |
| HG5F.15 | The 15 s cap fires | Hold BOOT for twenty seconds | Falling tone at fifteen seconds without releasing; the upload happens then |
| HG5F.16 | Retries and the timeout | Stop the hub, then hold BOOT and speak | Three attempts, then `every attempt failed`, error tone, back to Idle within 25 s |
| HG5F.17 | No secret is logged | Search the device serial log and the hub log | The hub token appears in neither. No transcript, no reply text, no audio bytes |
| HG5F.18 | The mute survives a power cut | Mute in Settings, pull power, power on | Boots muted |
| HG5F.19 | Navigation is unharmed | Walk every row of `tests/nav-map.json` | Same behaviour as HG4.7 recorded, with push-to-talk compiled in |
| HG5F.20 | The dashboard is unharmed | Push a frame and repaint | Unchanged from HG6 |

Rollback, if any of this makes the device worse to live with: reflash the
backup recorded under HG2. The mute defaulting to on means a device that
arrives in a bad state can be left alone safely in the meantime.

## HG6 Refresh timing

| # | Check | Expected evidence |
| --- | --- | --- |
| HG6.1 | Full refresh duration distribution | At least 20 samples of `timing_ms.panel` from `/api/v1/dashboard/status` |
| HG6.2 | Worst observed refresh against the 120 s BUSY timeout | The maximum sample, and whether any BUSY timeout was counted |
| HG6.3 | Partial-window refresh works on this BWRY panel | `RefreshRect` on the quick-switch bounds: does the overlay appear without ghosting the rest |
| HG6.4 | If HG6.3 fails, the fallback is acceptable | Full refresh on overlay open and close, timed. Slow but honest |
| HG6.5 | A click during a refresh is applied afterwards | Press DOWN during a refresh; log shows the intent held, then applied |
| HG6.6 | Five clicks during a refresh move one page | Coalescing observed, not just unit-tested |

## HG7 Wake word

Only after HG5, and only if the partition question is answered.

| # | Check | Expected evidence |
| --- | --- | --- |
| HG7.1 | A model partition exists in the layout | New `partitions/v2/16m.csv` and what was shrunk to make room |
| HG7.2 | `srmodels.bin` is generated and flashed | The file, its size, and the flash record |
| HG7.3 | Detection rate in the room it will live in | Hit and miss counts over a stated number of attempts |
| HG7.4 | False-accept rate over a normal evening | Hours observed, false wakes counted |
| HG7.5 | Internal SRAM headroom with AFE running | Free internal heap with the AFE active, against the 64 KB PSRAM malloc reserve |
| HG7.6 | Energy cost | Measured current with AFE always on, against the 30-minute deep-sleep design |
| HG7.7 | Licensing decision recorded | Which model, and the trademark position for it |

See `docs/VOICE-FEASIBILITY.md` for why every one of these is currently blocked.

---

## How to fill this in

One row at a time, with the evidence attached. A row without evidence is not
done. A row that was attempted and failed is recorded as failed, with what
happened, because a failed check is more informative than a missing one and far
more informative than an optimistic one.
