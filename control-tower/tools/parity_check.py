#!/usr/bin/env python3
"""Maintainer-only, read-only parity check of pack() against a second renderer.

MAINTAINER TOOL. It is not part of `npm run test`, it is not needed to run or
develop this project, and it will do nothing useful unless you happen to have
the other renderer this project was cross-checked against. It is kept because
the check it performs — that two independent implementations pack the same
30000 bytes — is the strongest evidence this repository has that the panel is
painted correctly, and deleting the tool would delete the ability to repeat it.

    python3 tools/parity_check.py

What it does: renders a set of fixture frames with the TypeScript renderer,
then re-packs each one with the reference implementation's pack() and compares the
30000 bytes. A single differing byte means the tower would paint the wrong
colours on the panel.

Safety, deliberately narrow:

  * It imports the reference module by path, as a pure module. That module's
    top level only defines constants, a threading.Event and an RLock. It does
    NOT call cycle(), does NOT read or write runtime/, and does NOT take
    runtime/scheduler.lock.
  * Only pack() is called. No network, no HA, no EventKit, no device.
  * Nothing is written outside a temporary directory.

This is a manual cross-check, not part of `npm run test`: it depends on a
checkout of the reference renderer being present.
"""

from __future__ import annotations

import importlib.util
import os
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
# The reference implementation to compare against. Nothing is assumed about
# where it lives: pass a path, or set NOTE4C_REFERENCE_RENDERER. With neither,
# the script says so and exits without comparing anything.
def _reference_path() -> pathlib.Path | None:
    if len(sys.argv) > 1:
        return pathlib.Path(sys.argv[1]).expanduser()
    from_env = os.environ.get("NOTE4C_REFERENCE_RENDERER", "").strip()
    return pathlib.Path(from_env).expanduser() if from_env else None


COMPOSER = _reference_path()


def load_composer():
    if COMPOSER is None:
        print(
            "No reference renderer given. Pass a path to it, or set "
            "NOTE4C_REFERENCE_RENDERER. Nothing was compared."
        )
        return None
    if not COMPOSER.exists():
        print(f"Composer not found at {COMPOSER}; nothing to compare against.")
        return None
    # Import by path so the composer package layout stays untouched.
    sys.path.insert(0, str(COMPOSER.parent))
    spec = importlib.util.spec_from_file_location("note4c_dashboard_ref", COMPOSER)
    if spec is None or spec.loader is None:
        raise RuntimeError("Could not build an import spec for dashboard.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main() -> int:
    composer = load_composer()
    if composer is None:
        return 0

    from PIL import Image

    with tempfile.TemporaryDirectory(prefix="note4c-parity-") as tmp:
        result = subprocess.run(
            ["npx", "tsx", "tools/export_parity_fixtures.ts", tmp],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        if result.returncode != 0:
            print(result.stdout)
            print(result.stderr, file=sys.stderr)
            return 1

        names = [line.strip() for line in result.stdout.splitlines() if line.strip()]
        failures = 0

        for name in names:
            image = Image.open(pathlib.Path(tmp) / f"{name}.png").convert("RGB")
            reference = composer.pack(image)
            ours = (pathlib.Path(tmp) / f"{name}.bin").read_bytes()

            if reference == ours:
                print(f"  ok    {name}  ({len(ours)} bytes identical)")
                continue

            failures += 1
            first = next(
                (i for i, (a, b) in enumerate(zip(reference, ours)) if a != b), None
            )
            print(
                f"  FAIL  {name}: first difference at byte {first} "
                f"(composer {reference[first]:#04x}, tower {ours[first]:#04x})"
            )

        print()
        if failures:
            print(f"{failures} of {len(names)} fixtures differ.")
            return 1
        print(f"All {len(names)} fixtures pack identically to the composer.")
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
