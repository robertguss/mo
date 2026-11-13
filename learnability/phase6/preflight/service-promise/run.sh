#!/usr/bin/env bash
# Rebuilds the baseline (02822786) and the prototype (02822786 + prototype.patch) in clean copies
# exported from git, then runs every result in README.md into out/.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
repo=$(git -C "$here" rev-parse --show-toplevel)
base=02822786
work=$(mktemp -d)
out="$here/out"
mkdir -p "$out"

for tree in baseline prototype; do
    mkdir -p "$work/$tree"
    git -C "$repo" archive "$base" | tar -x -C "$work/$tree"
done
(cd "$work/prototype" && patch -p1 --quiet <"$here/prototype.patch")
for tree in baseline prototype; do
    make -C "$work/$tree" CC=gcc -j8 >"$work/$tree.build.log" 2>&1
done
make -C "$work/prototype" CC=gcc -j8 build/gcc/debug/tests/test_recovery.test.run \
    build/gcc/debug/tests/test_recovery_faults.test.run 2>&1 | grep -v '^gcc' >"$out/unit-tests.txt"

old="$work/baseline/build/gcc/debug/mo"
new="$work/prototype/build/gcc/debug/mo"
cp -r "$here/promised" "$here/faulty" "$work/"
cp -r "$work/promised" "$work/mutant-finished"
sed -i 's/^    Ready | Done | Dead: Error(NotLeased(id: id))$/    Done: Ok(job)\n    Ready | Dead: Error(NotLeased(id: id))/' \
    "$work/mutant-finished/job-queue.mo"
cp -r "$work/promised" "$work/mutant-submit"
sed -i 's/Job(id: id, payload: payload, attempts: 0, state: Ready))$/Job(id: id, payload: payload, attempts: 1, state: Ready))/' \
    "$work/mutant-submit/job-queue.mo"
diff "$work/promised/job-queue.mo" "$work/mutant-finished/job-queue.mo" >"$out/mutant-finished.diff" || true
diff "$work/promised/job-queue.mo" "$work/mutant-submit/job-queue.mo" >"$out/mutant-submit.diff" || true
cp -r "$repo/services/task-queue" "$work/original"

check() { # binary program output
    local status=0
    (cd "$work" && "$1" test "$2/main.mo" --only Main.service.recovery --calls 200 \
        --seconds 100000) >"$out/$3.txt" 2>&1 || status=$?
    echo "exit $status" >>"$out/$3.txt"
}
check "$old" faulty baseline-faulty
check "$new" faulty prototype-faulty-1
check "$new" faulty prototype-faulty-2
check "$new" promised prototype-promised
check "$new" mutant-finished prototype-mutant-finished
check "$new" mutant-submit prototype-mutant-submit
check "$old" original baseline-original
check "$new" original prototype-original
if cmp -s "$out/prototype-faulty-1.txt" "$out/prototype-faulty-2.txt"; then echo identical; else echo different; fi >"$out/determinism.txt"

: >"$out/cost.txt"
for run in 1 2 3; do
    for pair in "$old original baseline-original" "$new original prototype-no-promises" \
        "$new promised prototype-three-promises"; do
        set -- $pair
        (cd "$work" && /usr/bin/time -v "$1" test "$2/main.mo" --only Main.service.recovery \
            --calls 200 --seconds 100000 2>"$work/time.txt" >/dev/null)
        awk -v name="$3" -v run="$run" -F': ' '
            /Elapsed \(wall clock\)/ {wall = $2}
            /User time/ {user = $2}
            /Maximum resident/ {rss = $2}
            END {printf "%s run %s  wall %s  user %ss  max RSS %s KB\n", name, run, wall, user, rss}
        ' "$work/time.txt" >>"$out/cost.txt"
    done
done
cat "$out"/*.txt
