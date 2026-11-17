#!/usr/bin/env bash
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
repo=$(git -C "$here" rev-parse --show-toplevel)
base=$(git -C "$repo" rev-parse --short=8 "${BASE:-2b6335c3}")
port=${PORT:-18123}
runs=${RUNS:-5}
work=$(mktemp -d)
out="$here/${OUT:-out}"
mkdir -p "$out"

git -C "$repo" archive "$base" | tar -x -C "$work"
(cd "$work" && patch -p1 --quiet <"$here/prototype.patch")
make -C "$work" CC=gcc -j"$(nproc)" build/gcc/debug/mo >"$work/debug.log" 2>&1
make -C "$work" CC=gcc -j"$(nproc)" BUILD=build/optimized DEBUG='-O2 -g -pthread' \
    build/optimized/debug/mo >"$work/optimized.log" 2>&1

cp -r "$here/proposed" "$here/today" "$here/parse-only" "$work/"
for size in 1048576 102400; do
    python3 "$here/server.py" --body "$size" >"$work/parse-only/body-$size.json"
done
python3 "$here/server.py" "$port" &
server=$!
trap 'kill $server; rm -rf "$work"' EXIT
sleep 1

runs_file="$out/runs.txt"
{
    echo "host: $(nproc) vCPUs, $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | sed 's/^ //'), $(free -g | awk '/Mem:/{print $2}') GiB, $(uname -r), $(gcc --version | head -1)"
    echo "source: $base + prototype.patch"
    echo "body: $(wc -c <"$work/parse-only/body-1048576.json") bytes for 1048576, $(wc -c <"$work/parse-only/body-102400.json") bytes for 102400"
    echo "columns: build variant case run exit wall_seconds max_rss_kb instructions heap_peak_bytes output"
} >"$runs_file"

measure() {
    local build=$1 variant=$2 name=$3
    shift 3
    for run in $(seq "$runs"); do
        local status=0
        (cd "$work" && MO_PROTOTYPE_STATS=1 /usr/bin/time -f "%e %M" -o "$work/time" "$@" \
            >"$work/stdout" 2>"$work/stderr") || status=$?
        read -r wall rss < <(tail -1 "$work/time")
        read -r _ _ instructions _ peak < <(grep '^prototype:' "$work/stderr" || echo "- - 0 - 0")
        echo "$build $variant $name $run $status $wall $rss $instructions $peak $(tr '\n' ' ' <"$work/stdout")" >>"$runs_file"
    done
}

for build in gcc optimized; do
    mo="$work/build/$build/debug/mo"
    label=$([ "$build" = gcc ] && echo debug || echo optimized)
    for size in 1048576 102400; do
        for variant in proposed today; do
            for kind in fixed chunked; do
                measure "$label" "$variant" "$kind-$size" "$mo" run "$variant/fetch.mo" "$port" "/$kind/$size"
            done
        done
        measure "$label" parse-only "file-$size" "$mo" run parse-only/parse.mo "parse-only/body-$size.json"
    done
done

python3 - "$runs_file" >"$out/summary.md" <<'PY'
import statistics
import sys

rows = {}
for line in open(sys.argv[1]).read().splitlines()[4:]:
    build, variant, case, _, status, wall, rss, work, peak, *_ = line.split()
    rows.setdefault((build, variant, case), []).append(
        (int(status), float(wall), int(rss), int(work), int(peak))
    )
print("| Build | Variant | Case | Runs | Exits | Wall, median (min to max) | Instructions | Heap peak | Max RSS |")
print("| --- | --- | --- | --- | --- | --- | --- | --- | --- |")
for (build, variant, case), runs in rows.items():
    walls = [r[1] for r in runs]
    exits = ", ".join(str(e) for e in sorted({r[0] for r in runs}))
    print(
        f"| {build} | {variant} | {case} | {len(runs)} | {exits} | "
        f"{statistics.median(walls):.2f} s ({min(walls):.2f} to {max(walls):.2f}) | "
        f"{statistics.median(r[3] for r in runs):,.0f} | "
        f"{statistics.median(r[4] for r in runs) / 2**20:.1f} MiB | "
        f"{statistics.median(r[2] for r in runs) / 1024:.0f} MiB |"
    )
PY
cat "$out/summary.md"
