# Local wake word: feasibility, not a promise

This document exists so that "the device could listen for a wake word" is
recorded with its costs attached rather than as an intention. Every figure below
was read out of this repository at the Milestone A commit. Nothing here has been
tried on hardware.

**Verdict: plausible on paper, blocked in practice.** It requires a
partition-table change, which requires a full backup-and-flash gate, which
requires hardware access this project has not been given. It also requires audio
hardware that has never been demonstrated to work at all. Push-to-talk comes
first, and even that is Milestone B.

---

## 1. The blocker: there is no room

`partitions/v2/16m.csv` allocates all 16 MiB with nothing left over:

| Partition | Offset | Size |
| --- | --- | --- |
| nvs | 0x9000 | 16 KiB |
| otadata | 0xd000 | 8 KiB |
| phy_init | 0xf000 | 4 KiB |
| ota_0 | 0x20000 | 4128 KiB |
| ota_1 | (next) | 4128 KiB |
| assets | 0x800000 | 8 MiB |

The last byte of `assets` is the last byte of the chip: 0x800000 + 8 MiB =
0x1000000. There is no gap to put a `model` partition in.

A wake word model needs one. `CONFIG_MODEL_IN_FLASH=y` is already set
(`sdkconfig:978`), and `srmodels.bin` is **not** in `build/`: verified absent.
The build never generates it, because there is nowhere to flash it to.

Making room means one of:

- **Shrink both OTA slots.** The app is currently 0x2b0140 bytes in a 0x3f0000
  slot, about 68 percent full. Both slots must shrink together, and the
  remaining headroom then caps every future firmware version.
- **Shrink `assets`.** That is the SPIFFS partition holding photos and the
  dashboard frame slots. Shrinking it means deciding what is deleted.

Either changes the partition table, which means the full HG2 and HG3 gate in
`docs/HARDWARE-ACCEPTANCE.md`: complete 16 MiB backup with its hash, partition
readback, parent approval quoting the artifact hash, and a rollback command
written down before anything is written. That gate exists because a mistake here
bricks the device, and this project has so far declined to open it.

## 2. What is already on disk

esp-sr 2.2.2 is vendored under `managed_components/espressif__esp-sr/`, with
prebuilt wakenet models present. English-capable ones include `wn9_hiesp`
(292 KiB) and `wn9_alexa`; the smaller `wn9s_hiesp` is 132 KiB.

`CONFIG_SR_WN_WN9_NIHAOXIAOZHI_TTS=y` is set (`sdkconfig:1016`) and has no
effect: with no model partition and no `srmodels.bin`, nothing is loaded. That
setting is a leftover from the upstream project, not a working configuration.

On the firmware side there is nothing at all: no `WakeWord` subclass exists,
`wake_word_` is never assigned, and the only audio processor is
`NoAudioProcessor` (`main/audio/audio_service.cc`). Three places would have
dereferenced null if a wake word had been wired in carelessly; those are now
guarded (`audio_service.cc:217, 447, 452`), which removes a crash, not a
blocker.

## 3. RAM

The AFE needs roughly 0.5 to 1 MiB, which PSRAM can supply: the board has 8 MiB
of octal PSRAM. The real risk is internal SRAM, not PSRAM.
`CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=65536` (`sdkconfig:2686`) reserves 64 KiB
of internal memory for allocations that cannot live in PSRAM: DMA buffers, Wi-Fi,
and the I2S path the AFE itself would use. Whether an always-running AFE fits
alongside the existing Wi-Fi, HTTP server and 30000-byte frame buffers is a
measurement (HG7.5), not an estimate.

## 4. Licensing

esp-sr is Espressif-MIT: free to use, on Espressif chips only, which is what
this is.

The model choice carries a separate question. `wn9_alexa` and the `jarvis`
variants use trademarked wake phrases; shipping them in a product is a
trademark question and not a licensing one. `wn9_hiesp` ("Hi ESP") is the safe
bundled English default.

A custom phrase, "Marvin" for instance, is not something this repository can
build. It requires Espressif's off-repo customization process, which means
sending them a request and receiving a model back. `wn9_customword` is present
in the component but is a placeholder for that process, not a way around it.

## 5. Energy

Unknown, and this is the part most likely to make the whole idea unattractive.

The device is designed to deep-sleep after 30 minutes and to wake only on the
BOOT key (`main/application.cc:620-700`). A wake word means the microphone, the
codec and the AFE run continuously, which is the opposite design. Nobody has
measured what that costs on this board, and no answer can be given until HG7.6.

The honest framing: a wake word may well turn a device that lasts days on a
charge into one that lasts hours. That trade may be worth making, but it has to
be measured before it is offered.

## 6. The microphone

One mono microphone, no acoustic echo cancellation reference. The ES8311 pin
map, its I2C address, the amplifier polarity and whether the microphone captures
anything usable are all unverified: HG5 in `docs/HARDWARE-ACCEPTANCE.md`.

The GPIO46 conflict this section used to describe is resolved in code. The codec
owns the pin; `BoardPowerBsp` no longer configures it and its `PowerAmpOn/Off`
methods are gone. That removes a latent double-owner, and it verifies nothing:
the polarity on the physical board is HG5.3b and is still open.

A wake word on a microphone nobody has heard is not a feature, it is a guess.

## 7. Policy, whatever the answer turns out to be

If a wake word ever ships here:

- **Off by default.** Opt-in from Settings, never on after an update.
- **Local only.** Detection on the device; no audio is streamed anywhere while
  waiting for a phrase.
- **The software mute is honoured**, and is described as a software mute. This
  firmware cannot cut power to the microphone, and will not claim to.
- **No ambient streaming, ever**, wake word or not.

## 8. Order of work

1. HG1 and HG5: does any of this hardware do anything. Push-to-talk is now
   compiled in and muted by default, so turning the mute off on the physical
   device is how this step gets run (HG5F).
2. Push-to-talk itself: written, host-tested and compiled, never heard. The
   user holds a button and knows they are being recorded, which is the honest
   version of voice input and needs no model partition.
3. Only then, if it is still wanted: HG7, the partition decision, and the energy
   measurement.

Push-to-talk existing does not move the wake word any closer. Everything in
sections 1 through 5 above is unchanged: there is still no model partition,
still no free flash, and still no measurement of what an always-running AFE
costs. Until step 1 produces evidence, this document is the whole of the wake
word feature.
