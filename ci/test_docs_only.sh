#!/bin/sh
# Tests ci/docs_only.sh: it says "true" only when every changed file is one no check reads.
set -eu

fail=0
expect() {
    want=$1
    shift
    got=$(printf '%s\n' "$@" | sh ci/docs_only.sh -)
    if [ "$got" != "$want" ]; then
        echo "error: ci/docs_only.sh said $got, expected $want, for: $*" >&2
        fail=1
    fi
}

expect true docs/status/v3-fuzz-log.md
expect true docs/design/architecture.md README.md AGENTS.md
expect true .agents/skills/ship-task/SKILL.md learnability/final/results.md
expect false docs/agent-guide.md
expect false docs/reference/README.md
expect false docs/design/library.md
expect false docs/status/v3-fuzz-log.md src/base/base.c
expect false .github/workflows/check.yml
expect false ci/check.sh
expect false tests/golden/example/main.mo
expect false Makefile

if [ "$(printf '' | sh ci/docs_only.sh -)" != false ]; then
    echo "error: ci/docs_only.sh said true for an empty change" >&2
    fail=1
fi

exit "$fail"
