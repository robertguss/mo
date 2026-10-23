#!/bin/sh
# Tests ci/ci_groups.sh: docs-only changes run no group, every other change runs the fast groups,
# and the nightly tier names the rest.
set -eu

fast='["gcc", "clang", "static", "drivers"]'
nightly='["cbmc", "lean", "fuzz", "sanitize"]'
fail=0
expect() {
    want=$1
    shift
    got=$(printf '%s\n' "$@" | sh ci/ci_groups.sh -)
    if [ "$got" != "$want" ]; then
        echo "error: ci/ci_groups.sh said $got, expected $want, for: $*" >&2
        fail=1
    fi
}

expect '[]' docs/status/v3-fuzz-log.md
expect '[]' AGENTS.md README.md
expect '[]' docs/plans/2026-10-22-1510-refactor-ci-two-tier-gate-plan.md
expect "$fast" docs/agent-guide.md
expect "$fast" src/check/check.c
expect "$fast" src/vm/vm.c tests/golden/example/main.mo
expect "$fast" lib/json.mo docs/status/v3-fuzz-log.md
expect "$fast" src/bytecode/read.c
expect "$fast" cbmc/reader.c
expect "$fast" ci/check.sh
expect "$fast" Makefile
expect "$fast" .github/workflows/check.yml
expect "$fast" ''

got=$(sh ci/ci_groups.sh --fast)
if [ "$got" != "$fast" ]; then
    echo "error: ci/ci_groups.sh --fast said $got, expected $fast" >&2
    fail=1
fi

got=$(sh ci/ci_groups.sh --nightly)
if [ "$got" != "$nightly" ]; then
    echo "error: ci/ci_groups.sh --nightly said $got, expected $nightly" >&2
    fail=1
fi

# The two tiers together must name every group ci/check.sh has, each once, or a group would never
# run in CI. qgate is the exception: it only reports, so the workflow runs it as its own job.
groups=$(sed -n 's/^all_groups=" \(.*\) "$/\1/p' ci/check.sh | sed 's/ *qgate//')
listed=$(printf '%s %s' "$fast" "$nightly" | tr -d '[]",')
if [ "$(printf '%s\n' $groups | sort)" != "$(printf '%s\n' $listed | sort)" ]; then
    echo "error: ci/check.sh has groups $groups but the two tiers run $listed" >&2
    fail=1
fi

exit "$fail"
