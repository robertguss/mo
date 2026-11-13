#!/usr/bin/env bash
# Times the task queue's checks under a fixed plan on this host, with and without the three
# service promises of ../service-promise, and writes out/timing.txt.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
repo=$(git -C "$here" rev-parse --show-toplevel)
base=02822786
work=$(mktemp -d)
out="$here/out"
mkdir -p "$out"
git -C "$repo" archive "$base" | tar -x -C "$work"
(cd "$work" && patch -p1 --quiet <"$here/../service-promise/prototype.patch")
make -C "$work" CC=gcc -j"$(nproc)" build/gcc/debug/mo >"$work/build.log" 2>&1
mo="$work/build/gcc/debug/mo"
cp -r "$here/../service-promise/promised" "$work/"
cp -r "$repo/services/task-queue" "$work/original"

report="$out/timing.txt"
{
    echo "host: $(nproc) vCPUs, $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | sed 's/^ //'), $(free -g | awk '/Mem:/{print $2}') GiB, $(uname -r), $(gcc --version | head -1)"
    echo "build: $base + service-promise/prototype.patch, gcc debug, as the task queue was deployed"
} >"$report"

measure() { # label repeats command...
    local label=$1 repeats=$2
    shift 2
    for run in $(seq "$repeats"); do
        (cd "$work" && /usr/bin/time -f "%e %U %M" "$@" >/dev/null 2>"$work/err" || true)
        read -r wall user rss < <(tail -1 "$work/err")
        echo "$label run $run  wall ${wall}s  cpu ${user}s  max_rss ${rss}KB" >>"$report"
    done
}

measure "check main.mo" 3 "$mo" check original/main.mo
for n in 200 1000; do
    measure "test original, $n sequences" 3 "$mo" test original/main.mo --calls "$n" --seconds 100000
    measure "test with three service promises, $n sequences" 3 "$mo" test promised/main.mo --calls "$n" --seconds 100000
done
measure "test queue-tests.mo (not imported by main.mo), default plan" 1 "$mo" test original/queue-tests.mo
measure "default 2-second cap, original" 1 "$mo" test original/main.mo
(cd "$work" && "$mo" test original/main.mo --only Main.service.recovery --json) |
    grep -o '"sequences":[0-9]*' | sed 's/^/default 2-second cap reached /' >>"$report"
rm -rf "$work"
