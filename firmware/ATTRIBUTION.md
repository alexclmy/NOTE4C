# Attribution

This branch is derived work built on top of a third-party firmware.

## Upstream

- Repository: <https://github.com/LazyYoun/youn-ink-fourcolor-firmware>
- Branch: `2bp`
- Base revision: `51812e4ab3fa80ba7a5a5a274635ca2cf3901a25`
  ("docs: remove internal remote reference", 2026-08-22T14:13:51+08:00)
- Licence: MIT (see `LICENSE` at the repository root and `firmware/LICENSE`)

The base revision was confirmed against the live remote with `git ls-remote`
before cloning: `51812e4…` was the tip of both `HEAD` and `refs/heads/2bp`.

The upstream project is itself derived from the xiaozhi ESP32 firmware, and
carries its own bundled components (`main/components/78__*`) plus a large set of
managed components resolved from the Espressif component registry. Their
licences are unchanged and their copyright headers are intact.

## Relationship to the firmware currently installed on the device

The device is running `notellm-2bp-v6.5.9-2bp-3fbbabb-20260705-mergebin.bin`,
built from commit `3fbbabb` on 2026-07-05.

**That commit is not reachable in the public repository.** The public history of
`2bp` begins on 2026-07-07 with a squashed re-publication, so `3fbbabb` predates
everything that was published. It cannot be checked out, and the baseline built
here is therefore *not* byte-identical to what is on the device.

The base used instead, `51812e4`, is the latest public tip. Among the eight
public commits it includes two that matter for this work:

- `e914177` "fix: scan full 2bpp frame for display changes"
- `40c7b6c` "fix: wait for DHCP before accepting WiFi provisioning"

Practical consequence: behavioural differences observed after flashing may
originate upstream rather than in this branch. `evidence/baseline/` holds an
unmodified build of `51812e4` and its hashes precisely so the two can be told
apart.

## Changes made in this branch

See `PROGRESS.md` and `REPORT.md` in the workspace root, and
`firmware/docs/DASHBOARD_API.md` and `firmware/docs/PROVISIONING.md` for the
added interfaces. Modified files keep their original copyright headers; files
added by this project carry `SPDX-License-Identifier: MIT`, matching upstream.
