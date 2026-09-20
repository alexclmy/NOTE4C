#!/usr/bin/env bash
# NOTE4C Lot 1 — replay script.
#
# Lot 1 could NOT be executed in the agent session of 2026-09-18: `git`, `bash <script>`,
# `cp -R`, `rsync`, `shasum` and `python3 -c` are all refused by that environment's
# permission layer (only `ls`, `mkdir`, `rm`, single-file `cp`, `find`, `du`, `diff`
# and `python3 --version` were permitted). Nothing was worked around.
#
# This script is the exact command sequence Lot 1 specifies, reviewed against the real
# source trees. Run it from a shell that HAS git and recursive copy. It is idempotent
# only in the sense that it refuses to run against a non-empty repo.
#
# It writes ONLY inside $REPO. It never touches the two source trees.

set -euo pipefail

REPO=/Users/marvin/projects/tools/note4c-fw-v1
UPSTREAM=/Users/marvin/projects/tools/note4c-firmware/upstream
ASINSTALLED=/Users/marvin/projects/tools/note4c-firmware-increments
TOOLCHAIN=/Users/marvin/projects/tools/note4c-firmware/toolchain/activate.sh

EXCLUDES=(
  --exclude=.git
  --exclude=__pycache__
  --exclude=firmware/build-hardware-lot1
  --exclude=firmware/build-hardware-lot2
  --exclude=firmware/build-hardware-lot2-capability-fix
  --exclude=firmware/build-hardware-lot3
  --exclude=firmware/build-hardware-lot3-final-rc
  --exclude=firmware/build-hardware-lot3-network-welcome-fix
  --exclude=firmware/build-hardware-lot3-status-stack-fix
  --exclude=firmware/build-hardware-lot3-welcome-fix
  --exclude=firmware/build-source-baseline
  --exclude=firmware/build-source-baseline-s3
  --exclude=firmware/managed_components   # 465 MB, gitignored, refetched by idf.py
)

die() { printf 'ABORT: %s\n' "$*" >&2; exit 1; }

# --- guard rails -------------------------------------------------------------
[ -d "$UPSTREAM" ]    || die "missing upstream tree $UPSTREAM"
[ -d "$ASINSTALLED" ] || die "missing as-installed tree $ASINSTALLED"
[ -e "$REPO/.git" ]   && die "$REPO already has a .git; refusing to replay"

# The as-installed tree is a dirty worktree and is EVIDENCE. Read-only, always.
# (rsync below never writes to it; nothing else in this script touches it.)

# --- T1: init + commit 1 -----------------------------------------------------
mkdir -p "$REPO"
git -C "$REPO" init -b main
# A fresh session may have no git identity; commits must not depend on one.
git -C "$REPO" config user.name  "NOTE4C Lot 1"
git -C "$REPO" config user.email "lot1@note4c.invalid"
rsync -a "${EXCLUDES[@]}" "$UPSTREAM"/ "$REPO"/
# Fable amendment (review, 2026-09-18): the three Lot 1 meta files already live in
# $REPO. A bare `add -A` would drag them into commit 1 (supposed to be a pure upstream
# copy) and leave commit 3 empty, which aborts under `set -e`. Exclude them here and in T2.
META_EXCLUDE=(':(exclude)PROVENANCE-MANIFEST.md' ':(exclude)provenance-manifest.json' ':(exclude)REPLAY-LOT1.sh')
git -C "$REPO" add -A -- . "${META_EXCLUDE[@]}"
git -C "$REPO" commit -m 'upstream youn-ink-fourcolor-firmware 2bp @ 51812e4a (copy)'

# --- T2: commit 2 = as-installed snapshot (delete-what-is-gone semantics) ----
# --delete makes the working tree EXACTLY the snapshot, so a file removed on the
# as-installed side is removed here too. Per the manifest there are currently 0 such
# files, but the flag is what makes commit 2 a snapshot rather than an overlay.
# The three Lot 1 meta files live only in $REPO; excluding them protects them from
# --delete (rsync does not delete excluded paths unless --delete-excluded is given).
rsync -a --delete "${EXCLUDES[@]}" \
  --exclude=PROVENANCE-MANIFEST.md \
  --exclude=provenance-manifest.json \
  --exclude=REPLAY-LOT1.sh \
  --exclude=lot1-replay.log \
  --exclude=lot1-hashes \
  --exclude=lot1-logs \
  "$ASINSTALLED"/ "$REPO"/
# Test fixtures are *.bin, which .gitignore:60 would swallow. They are test inputs,
# not artefacts: force-add them or the host suites are not reproducible from the repo.
git -C "$REPO" add -A -- . "${META_EXCLUDE[@]}"
git -C "$REPO" add -f firmware/tests/fixtures 2>/dev/null || true
git -C "$REPO" commit -m 'as-installed snapshot from dirty worktree, 2026-09-18'

# Sanity: the delta this produces MUST match provenance-manifest.json
# (74 added, 49 modified, 0 deleted; 122 tracked if firmware/sdkconfig stays ignored).
git -C "$REPO" diff --stat HEAD~1 HEAD | tail -1
git -C "$REPO" diff --name-status HEAD~1 HEAD | cut -f1 | sort | uniq -c

# --- T3: commit 3 = provenance manifest -------------------------------------
# The manifest files and this script survived T2 via the excludes above.
git -C "$REPO" add PROVENANCE-MANIFEST.md provenance-manifest.json REPLAY-LOT1.sh
git -C "$REPO" commit -m 'provenance manifest: upstream 51812e4a -> as-installed 2026-09-18'

# --- T4: commit 4 = the one authorised config change ------------------------
printf '\n# Lot 1: deterministic artefacts (no __DATE__/__TIME__, no absolute build paths)\nCONFIG_APP_REPRODUCIBLE_BUILD=y\n' \
  >> "$REPO/firmware/sdkconfig.defaults.esp32s3"
git -C "$REPO" add firmware/sdkconfig.defaults.esp32s3
git -C "$REPO" commit -m 'build: enable CONFIG_APP_REPRODUCIBLE_BUILD for the esp32s3 target'
# NOTE: firmware/sdkconfig is generated AND gitignored. Do not hand-edit it; let
# idf.py regenerate it from sdkconfig.defaults* on the next configure.

git -C "$REPO" log --oneline -4

# --- T5: host suites ---------------------------------------------------------
export PATH=/opt/homebrew/bin:$PATH
( cd "$REPO/firmware" && bash tests/host/run.sh )   # requires: ALL HOST SUITES PASSED
# A failure here is a STOP condition. Do not "repair" the snapshot.

# --- T6: reproducible double build ------------------------------------------
# Mechanism is a CMake option (firmware/main/CMakeLists.txt:179), not a Kconfig symbol.
# Hashes live under $REPO, not /tmp: agent sessions are sandboxed to
# /Users/marvin/projects/tools and cannot read/write /tmp (Fable amendment 2026-09-18).
HASHDIR="$REPO/lot1-hashes"
mkdir -p "$HASHDIR"
# Fable amendment (review, 2026-09-18), two build-fidelity fixes, both inside $REPO only:
# 1. firmware/sdkconfig was copied from the source trees but is generated AND gitignored;
#    if it exists, idf.py reuses it and IGNORES the T4 change in sdkconfig.defaults.esp32s3.
#    Remove it so idf.py regenerates from sdkconfig.defaults* (per the manifest's own note).
rm -f "$REPO/firmware/sdkconfig"
# 2. managed_components was excluded from the copy; refetching it via idf.py needs the
#    network. Pre-seed it read-only from the as-installed tree instead: zero outbound
#    network, and the exact dependency set the installed firmware was built with.
#    It is gitignored (.gitignore:43) so this cannot affect the commits or the delta.
rsync -a "$ASINSTALLED"/firmware/managed_components/ "$REPO"/firmware/managed_components/
# shellcheck disable=SC1090
source "$TOOLCHAIN"
cd "$REPO/firmware"

# Fable amendment (execution, 2026-09-18): with sdkconfig gone, idf.py falls back to
# target "esp32" and the build fails on esp32s3-only components. Nothing in
# sdkconfig.defaults* or build.sh pins the target; set it explicitly. set-target
# regenerates sdkconfig from sdkconfig.defaults* — which is what makes T4 take effect.
rm -rf build
idf.py set-target esp32s3

for variant in "off:" "on:-DVOICE_PTT_ENABLED=1"; do
  name=${variant%%:*}; flag=${variant#*:}
  for pass in 1 2; do
    rm -rf build
    # shellcheck disable=SC2086
    idf.py $flag build
    shasum -a 256 build/xiaozhi.bin | tee "$HASHDIR/voice-$name-pass$pass.sha256"
    ls -l build/xiaozhi.bin
  done
  diff "$HASHDIR/voice-$name-pass1.sha256" "$HASHDIR/voice-$name-pass2.sha256" \
    && echo "REPRODUCIBLE: voice-$name" \
    || die "NOT reproducible: voice-$name"
done
