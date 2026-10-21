#!/bin/sh
# Decides which ci/check.sh groups a pull request's change needs, for the hosted workflow.
#
#   ci/ci_groups.sh <base>   changed files are `git diff --name-only <base>...HEAD`
#   ci/ci_groups.sh -        changed files are read from standard input, one per line
#
# Prints the groups as a JSON array. A docs-only change (ci/docs_only.sh) needs none. Any other
# change needs every group except cbmc, which takes about fifteen minutes and reads only the
# bytecode reader, the arena, the verifier and the primitives: it runs when a changed file is one
# a CBMC harness in cbmc/ compiles (found with the compiler's -MM, so a new include or harness is
# covered without editing this file), is under cbmc/, or is part of how CI runs. Pushes to main and manual runs
# don't use this script; they run every group.
set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: ci/ci_groups.sh <base> | -" >&2
    exit 2
fi

cd "$(dirname "$0")/.."

if [ "$1" = "-" ]; then
    files=$(cat)
else
    files=$(git diff --name-only "$1"...HEAD)
fi

if [ "$(printf '%s\n' "$files" | sh ci/docs_only.sh -)" = true ]; then
    echo '[]'
    exit 0
fi

# Every file the harnesses in cbmc/ compile, one per line. Without a compiler, cbmc always runs.
reads=
for cc in gcc clang cc; do
    if command -v "$cc" >/dev/null 2>&1; then
        reads=$("$cc" -std=c17 -Isrc -MM cbmc/*.c | sed -e 's/^[^:]*://' -e 's/\\$//' | tr ' ' '\n' | sed '/^$/d')
        break
    fi
done

cbmc=false
if [ -z "$reads" ]; then cbmc=true; fi
while IFS= read -r file; do
    case "$file" in
    cbmc/* | ci/check.sh | ci/ci_groups.sh | Makefile | .agents/setup | .github/workflows/check.yml) cbmc=true ;;
    esac
    if [ -n "$reads" ] && printf '%s\n' "$reads" | grep -qxF -- "$file"; then cbmc=true; fi
done <<EOF2
$files
EOF2

groups='"gcc", "clang", "sanitize", "fuzz", "drivers", "lean", "static"'
if [ "$cbmc" = true ]; then groups="$groups, \"cbmc\""; fi
echo "[$groups]"
