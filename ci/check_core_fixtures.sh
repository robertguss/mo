#!/bin/sh
# Run after the Lean build. Export into disposable storage, never into the committed corpus:
# checking must fail, not silently update fixtures when the reference printer changes.
set -eu
tree=$(mktemp -d)
trap 'rm -rf "$tree"' EXIT
trap 'exit 1' HUP INT TERM
(cd formal && lake env lean --run ExportSamples.lean "$tree")
if ! diff -ru tests/core "$tree"; then
    echo 'error: Core fixtures differ from Lean; regenerate with formal/ExportSamples.lean' >&2
    exit 1
fi
echo 'Core fixtures: all 13 sample runs match the Lean exporter byte for byte'
