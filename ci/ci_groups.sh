#!/bin/sh
# Decides which ci/check.sh groups CI runs. The two tiers and why are in ci/orb/README.md.
#
#   ci/ci_groups.sh <base>      changed files are `git diff --name-only <base>...HEAD`
#   ci/ci_groups.sh -           changed files are read from standard input, one per line
#   ci/ci_groups.sh --fast      the fast groups, for a run with no range to look at
#   ci/ci_groups.sh --nightly   the slow groups the nightly workflow runs against main
#
# Prints the groups as a JSON array. A docs-only change (ci/docs_only.sh) needs none; any other
# change needs the fast groups, the ones the required `check` status waits for.
set -eu

fast='["gcc", "clang", "static", "drivers"]'
nightly='["cbmc", "lean", "fuzz", "sanitize"]'

if [ "$#" -ne 1 ]; then
    echo "usage: ci/ci_groups.sh <base> | - | --fast | --nightly" >&2
    exit 2
fi

case "$1" in
--fast) echo "$fast" && exit 0 ;;
--nightly) echo "$nightly" && exit 0 ;;
esac

cd "$(dirname "$0")/.."

if [ "$1" = "-" ]; then
    files=$(cat)
else
    files=$(git diff --name-only "$1"...HEAD)
fi

if [ "$(printf '%s\n' "$files" | sh ci/docs_only.sh -)" = true ]; then
    echo '[]'
else
    echo "$fast"
fi
