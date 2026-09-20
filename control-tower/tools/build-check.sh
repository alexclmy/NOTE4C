#!/usr/bin/env bash
#
# Verify that the project builds, without disturbing anything.
#
# Two things make a plain `next build` unsafe as a *check* in this repo:
#
#  1. It writes to `.next`, which is the directory a running `next start` is
#     reading chunks out of. A verification build therefore breaks the server
#     that is currently serving the dashboard.
#  2. It rewrites `next-env.d.ts` and reformats `tsconfig.json` to point at
#     whatever dist directory it just used. With NEXT_DIST_DIR set, that means
#     a check build leaves two tracked files pointing at a scratch directory
#     that is then deleted — which is exactly how `next-env.d.ts` came to
#     reference `.next-hybrid-review/types/routes.d.ts` in the working tree.
#
# So: build into a scratch directory, then put the two generated files back and
# remove the scratch tree. The script is the fix for (2) recurring; remembering
# to do it by hand is not.
#
# Three rules the script enforces rather than trusts:
#
#  - **The two files are restored from byte copies, not from git.** The previous
#    version ran `git checkout -- next-env.d.ts tsconfig.json`, which also threw
#    away any uncommitted edit to `tsconfig.json` — a new path alias, a stricter
#    flag, work in progress. A build check has no business reverting the
#    author's edits, and this one no longer can.
#  - **The scratch directory name is validated before anything is deleted.**
#    The cleanup path ends in `rm -rf`, so the argument is checked against one
#    narrow shape instead of being interpolated and hoped for.
#  - **An existing scratch directory is refused, not reused.** Reusing it would
#    mean deleting a directory this run did not create.
#
# Usage: tools/build-check.sh [dist-dir]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

DIST="${1:-.next-build-check}"

# One path segment, `.next-` prefixed, ordinary characters only. This rejects
# `.next` itself, absolute paths, anything with a slash, `..`, names beginning
# with a dash, and the empty string — which between them are every way the
# `rm -rf` below could reach something that is not this script's scratch tree.
# The prefix is also what `/.next-*/` in .gitignore matches, so the scratch tree
# can never appear as untracked noise.
if [[ ! "$DIST" =~ ^\.next-[A-Za-z0-9._-]+$ ]]; then
    cat >&2 <<EOF
Refusing to check-build into "$DIST".

The scratch directory must be a single path segment of the form .next-<name>
(letters, digits, dot, dash, underscore). That keeps it out of .next — which is
the directory a running server reads from — and inside the .gitignore rule for
scratch builds.

  tools/build-check.sh              # uses .next-build-check
  tools/build-check.sh .next-review

To build for real, use 'npm run build'.
EOF
    exit 1
fi

if [[ -e "$DIST" ]]; then
    echo "Refusing to check-build into the existing $DIST." >&2
    echo "This script deletes its scratch tree when it finishes, and it will not" >&2
    echo "delete a directory it did not create. Remove or rename $DIST first." >&2
    exit 1
fi

# Byte copies of the two files `next build` is about to rewrite, kept outside
# the repository so a half-finished run cannot leave them lying around in it.
KEEP="$(mktemp -d "${TMPDIR:-/tmp}/note4c-build-check.XXXXXX")"
GENERATED=(next-env.d.ts tsconfig.json)

for file in "${GENERATED[@]}"; do
    # An `if` rather than `[[ ... ]] && cp`: under `set -e` a false test as the
    # loop's last command would exit the script with the file unbuilt.
    if [[ -f "$file" ]]; then
        cp -p "$file" "$KEEP/$file"
    fi
done

# Everything below runs even if the build fails, because a failed build has
# already rewritten the two generated files by the time it fails.
cleanup() {
    local status=$?
    local file
    for file in "${GENERATED[@]}"; do
        if [[ -f "$KEEP/$file" ]]; then
            # Only write when the build actually changed it, so an untouched
            # file keeps its modification time and no watcher is woken.
            cmp -s "$KEEP/$file" "$file" || cp -p "$KEEP/$file" "$file"
        fi
    done
    rm -rf "$KEEP"
    rm -rf "${ROOT:?}/${DIST:?}"
    if [[ $status -eq 0 ]]; then
        echo
        echo "Build check passed. ${DIST} removed; next-env.d.ts and tsconfig.json restored."
    fi
    return $status
}
trap cleanup EXIT

NEXT_DIST_DIR="$DIST" npx next build
