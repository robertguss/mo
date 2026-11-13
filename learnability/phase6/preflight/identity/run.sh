#!/usr/bin/env bash
# Rebuilds mo and the prototype in a clean copy of 02822786 exported from git, then digests the
# task queue and every variant.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
repo=$(git -C "$here" rev-parse --show-toplevel)
root=$(mktemp -d)
git -C "$repo" archive 02822786 | tar -x -C "$root"
cd "$root"

make -j8 CC=gcc >/dev/null
mkdir -p build/identity "$here/out"
objects=$(find build/gcc/debug/src -name '*.o' ! -path '*/tools/main.o')
gcc -std=c17 -Isrc -Ibuild/generated -Wall -Wextra -Wshadow -Wconversion -Wsign-conversion -Werror \
    -O0 -g -pthread "$here/identity.c" $objects build/gcc/debug/vendor/sqlite/sqlite3.o \
    -o build/identity/identity -lm

run() {
    local name="$1" dir="build/identity/runs/$1/task-queue"
    rm -rf "build/identity/runs/$name"
    mkdir -p "$(dirname "$dir")"
    cp -R services/task-queue "$dir"
    if [ "$name" != base ]; then
        patch -s -p1 -d "$dir" <"$here/variants/$name.patch"
    fi
    build/identity/identity "$dir/main.mo" "$root/lib" >"$here/out/$name.json"
    local status=0
    build/gcc/debug/mo test "$dir/queue-tests.mo" >"build/identity/runs/$name/test.txt" 2>&1 ||
        status=$?
    local log="build/identity/runs/$name/test.txt"
    {
        echo "exit $status"
        grep -E '^(failed|stopped) ' "$log" | sed "s|$dir/||" || true
        grep -E '^[0-9]+ passed' "$log" || grep -m 1 -E '^error ' "$log" | sed "s|$dir/||"
    } >"$here/out/$name.test.txt"
    echo "$name: $(tail -n 1 "$here/out/$name.test.txt") (exit $status)"
}

run base
for patch in "$here"/variants/*.patch; do
    run "$(basename "$patch" .patch)"
done
python3 "$here/compare.py" "$here/out" >"$here/out/table.md"
cat "$here/out/table.md"
rm -rf "$root"
