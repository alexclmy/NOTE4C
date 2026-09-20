#!/usr/bin/env bash
#
# Build the tree that would be published, without publishing anything.
#
# This repository is the *working* one. It holds planning briefs, agent
# transcripts, verification logs and a `.hermes/` directory full of internal
# notes — material that is genuinely useful here and has no business in a
# public repository, both because it is noise and because it is full of paths,
# addresses and context about one household.
#
# So publication is a snapshot, not a push: this script copies the tracked
# tree minus the private material into `public-snapshot/`, and stops. It
# creates no repository, adds no remote, makes no commit and contacts nothing.
# What to do with the directory afterwards is a decision for a human at a
# terminal.
#
#   tools/public-snapshot.sh [destination]
#
# Afterwards, read it. `git ls-files` in the snapshot, `grep` for your own
# name, open the docs. The last check before publishing is a person looking.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

DEST="${1:-$ROOT/public-snapshot}"

# Everything here stays private. Paths are matched against the repository-
# relative path of each file considered for the snapshot.
#
# As of the release candidate these files are also untracked and ignored (see
# .gitignore), so `git ls-files` below no longer offers them in the first place
# and this list normally withholds nothing. That is the point: two independent
# mechanisms, either of which is sufficient. Loosening .gitignore does not leak
# them, and deleting an entry here does not leak them. The count printed at the
# end is therefore expected to read zero, and the check after the copy is what
# actually proves the tree is clean.
EXCLUDE_PREFIXES=(
    ".hermes/"
)
EXCLUDE_FILES=(
    "FABLE-PLANNING-BRIEF.md"
    "IMPLEMENTATION-BRIEF.md"
    "MODULE-EXPANSION-BRIEF.md"
    "MODULE-EXPANSION-REPORT.md"
    "TYPOGRAPHY-COMPLETION-BRIEF.md"
    "TYPOGRAPHY-FOLLOWUP-BRIEF.md"
    "TYPOGRAPHY-REPORT.md"
    "RUNBOOK-REAL-PUSH.md"
)

excluded() {
    local path="$1"
    local prefix file
    for prefix in "${EXCLUDE_PREFIXES[@]}"; do
        [[ "$path" == "$prefix"* ]] && return 0
    done
    for file in "${EXCLUDE_FILES[@]}"; do
        [[ "$path" == "$file" ]] && return 0
    done
    return 1
}

if [[ -e "$DEST" ]]; then
    echo "Refusing to write into an existing $DEST. Remove it first." >&2
    exit 1
fi

mkdir -p "$DEST"

copied=0
skipped=0

#
# `--cached --others --exclude-standard`: tracked files *and* files that are
# not yet committed but are not ignored either. A snapshot built from
# `git ls-files` alone silently omits every file added since the last commit —
# which, the first time this ran, was the entire open-source dossier. Ignored
# files (.env.local, node_modules, build output) are still excluded.
#
while IFS= read -r path; do
    if excluded "$path"; then
        skipped=$((skipped + 1))
        continue
    fi
    mkdir -p "$DEST/$(dirname "$path")"
    cp -p "$path" "$DEST/$path"
    copied=$((copied + 1))
done < <(git ls-files --cached --others --exclude-standard)

#
# Then prove it, rather than trusting the loop.
#
# The filter above answers "was this path offered and refused". This answers the
# question that actually matters — "is it in the tree I am about to publish" —
# and it would still catch a private file that arrived by some route nobody has
# thought of yet. It is deliberately the last thing that runs before the
# instructions, and it is fatal: a snapshot that cannot be shown to be clean is
# worth less than no snapshot.
#
leaked=0
for prefix in "${EXCLUDE_PREFIXES[@]}"; do
    if [[ -e "$DEST/${prefix%/}" ]]; then
        echo "LEAK: $DEST/$prefix exists in the snapshot." >&2
        leaked=$((leaked + 1))
    fi
done
for file in "${EXCLUDE_FILES[@]}"; do
    if [[ -e "$DEST/$file" ]]; then
        echo "LEAK: $DEST/$file exists in the snapshot." >&2
        leaked=$((leaked + 1))
    fi
done

if [[ $leaked -gt 0 ]]; then
    echo >&2
    echo "$leaked private path(s) reached $DEST. Not publishable. The snapshot" >&2
    echo "has been left in place so you can see what happened; delete it when" >&2
    echo "you are done looking." >&2
    exit 1
fi

echo
echo "Snapshot written to $DEST"
echo "  $copied files copied, $skipped withheld here, no private path present."
echo
echo "Nothing has been published. Before you do:"
echo "  1. grep the snapshot for your name, your addresses and your paths."
echo "  2. Fill in the contact placeholders in SECURITY.md and"
echo "     CODE_OF_CONDUCT.md."
echo "  3. Read README.md and QUICKSTART.md as a stranger would."
echo
echo "The working repository keeps its full history and every private file."
echo "Publishing from a new repository with fresh history is the recommended"
echo "route: the values extracted into environment variables are still in this"
echo "repository's history, and a public clone would carry them."
