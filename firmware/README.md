# NOTE4C firmware

The code that runs on the NOTE4C panel — a ZECTRIX ESP32-S3 devkit with a
400×300 four-color (BWRY) e-paper display. It renders and stores dashboard
frames, serves a small LAN API the [Control Tower](../control-tower/) pushes to,
manages battery power (deep sleep with a timer/button wake), and draws the
on-device UI with a dependency-light RawDraw pipeline.

> This is a **fork**. It is derived from an upstream ESP32-S3 e-paper firmware
> (itself derived from the xiaozhi ESP32 project). The lineage, base revision,
> bundled-component licences and build provenance are documented in
> [`ATTRIBUTION.md`](ATTRIBUTION.md) and
> [`PROVENANCE-MANIFEST.md`](PROVENANCE-MANIFEST.md). Upstream copyright notices
> are intact, including `firmware/LICENSE`.

## Layout

- [`firmware/`](firmware/) — the ESP-IDF project (sources under `main/`).
- [`firmware/docs/`](firmware/docs/) — the device contract: the
  [dashboard](firmware/docs/DASHBOARD_API.md) and
  [config](firmware/docs/CONFIG_API.md) APIs, [power](firmware/docs/POWER.md),
  [provisioning](firmware/docs/PROVISIONING.md), [storage](firmware/docs/STORAGE.md)
  and the [manual](firmware/docs/MANUAL.md).
- [`lot1-hashes/`](lot1-hashes/) — reference build hashes.

## Build

Requires [ESP-IDF](https://docs.espressif.com/projects/esp-idf/) (v6.x). From
`firmware/`:

```sh
idf.py set-target esp32s3
idf.py build                          # voice off (default)
idf.py -DVOICE_PTT_ENABLED=1 build    # voice on
```

The panel latches its own power: hold the power button through the first boot
after a flash, or the board cuts its rail. See
[`firmware/docs/HARDWARE-ACCEPTANCE.md`](firmware/docs/HARDWARE-ACCEPTANCE.md).

> ⚠️ Flashing replaces the running firmware. Back up the full 16 MiB flash first
> (`esptool read-flash 0 0x1000000 backup.bin`) so you can roll back.

## Host tests

The device logic is written to be testable off-target: the pure logic carries no
ESP-IDF headers in its `.h` files, and the host tests compile the real
translation units. From `firmware/`:

```sh
tests/host/run.sh
```

## License

[MIT](../LICENSE), with the upstream notice preserved in
[`firmware/LICENSE`](firmware/LICENSE). See [`ATTRIBUTION.md`](ATTRIBUTION.md).
