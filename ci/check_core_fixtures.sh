#!/bin/sh
# Run after the Lean build. Export into disposable storage, never into the committed corpus:
# checking must fail, not silently update fixtures when the reference printer changes.
set -eu
tree=$(mktemp -d)
trap 'rm -rf "$tree"' EXIT
trap 'exit 1' HUP INT TERM
(cd formal && lake env lean --run ExportSamples.lean "$tree/runs" "$tree/fuzz")
if ! diff -ru tests/core "$tree/runs" || ! diff -ru fuzz/corpus/fuzz_core_text "$tree/fuzz"; then
    echo 'error: Core fixtures differ from Lean; regenerate with formal/ExportSamples.lean' >&2
    exit 1
fi
echo 'Core fixtures: all 15 runs and 15 fuzz seeds match the Lean exporter byte for byte'
