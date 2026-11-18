#!/usr/bin/env bash
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
repo=$(git -C "$here" rev-parse --show-toplevel)
old=$(git -C "$repo" rev-parse --short=8 "${OLD:-2b6335c3}")
new=$(git -C "$repo" rev-parse --short=8 "${NEW:-HEAD}")
runs=${RUNS:-3}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$here/out"

build() {
    mkdir "$work/$1"
    git -C "$repo" archive "$2" | tar -x -C "$work/$1"
    (cd "$work/$1" && patch -p1 --quiet <"$here/../http-in-mo/prototype.patch")
    make -C "$work/$1" CC=gcc -j"$(nproc)" build/gcc/debug/mo >"$work/$1.log" 2>&1
}
build old "$old"
build new "$new"

python3 "$here/corpus.py" >"$work/corpus.txt" 2>"$work/count"
python3 "$here/../http-in-mo/server.py" --body 1048576 >"$work/compact.json"
python3 "$here/shapes.py" "$work/compact.json" "$work"
cp "$here/differ.mo" "$here/../http-in-mo/parse-only/parse.mo" "$work/"

{
    echo "old parser: $old"
    echo "new parser: $new"
    echo "corpus: $(cat "$work/count")"
    for side in old new; do
        (cd "$work" && "$side/build/gcc/debug/mo" run differ.mo corpus.txt >"$side.txt")
    done
    if cmp -s "$work/old.txt" "$work/new.txt"; then
        echo "corpus result: the two parsers print the same line for every document"
    else
        echo "corpus result: DIFFERENT"
        different=1
    fi
    echo "columns: parser shape bytes run exit user_seconds instructions heap_peak_bytes output"
    for shape in compact one-escape pretty escapes numbers; do
        for side in old new; do
            for run in $(seq "$runs"); do
                status=0
                (cd "$work" && MO_PROTOTYPE_STATS=1 /usr/bin/time -f "%U" -o time \
                    "$side/build/gcc/debug/mo" run parse.mo "$shape.json" >stdout 2>stderr) || status=$?
                read -r _ _ instructions _ peak < <(grep '^prototype:' "$work/stderr" || echo "- - 0 - 0")
                echo "$side $shape $(wc -c <"$work/$shape.json") $run $status $(tail -1 "$work/time") $instructions $peak $(tr '\n' ' ' <"$work/stdout")"
            done
        done
    done
} >"$here/out/results.txt"
cat "$here/out/results.txt"
exit "${different:-0}"
