#!/bin/sh
# Tests ci/ci_groups.sh: docs-only changes run no group, and cbmc runs only when the change
# touches what the CBMC harness compiles or how CI runs.
set -eu

all='["gcc", "clang", "sanitize", "fuzz", "drivers", "lean", "static", "cbmc"]'
most='["gcc", "clang", "sanitize", "fuzz", "drivers", "lean", "static"]'
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
expect "$most" docs/agent-guide.md
expect "$most" src/check/check.c
expect "$most" src/vm/vm.c tests/golden/example/main.mo
expect "$most" lib/json.mo docs/status/v3-fuzz-log.md
expect "$all" src/bytecode/read.c
expect "$all" src/base/limits.h
expect "$all" src/heap/types.h
expect "$all" cbmc/reader.c
expect "$all" cbmc/prove.sh
expect "$all" ci/check.sh
expect "$all" Makefile
expect "$all" ci/ci_groups.sh
expect "$all" .agents/setup
expect "$all" .github/workflows/check.yml
expect "$all" src/check/check.c src/bytecode/bytecode.h
expect "$most" ''

# Without a compiler to list what the harness reads, cbmc always runs.
nocc=$(mktemp -d)
for tool in sh sed tr grep cat dirname git; do ln -s "$(command -v "$tool")" "$nocc/$tool"; done
got=$(printf 'src/check/check.c\n' | PATH=$nocc sh ci/ci_groups.sh -)
rm -rf "$nocc"
if [ "$got" != "$all" ]; then
    echo "error: ci/ci_groups.sh without a compiler said $got, expected $all" >&2
    fail=1
fi

# $all must name every group ci/check.sh has, or a new group would never run on a pull request.
groups=$(sed -n 's/^all_groups=" \(.*\) "$/\1/p' ci/check.sh)
listed=$(printf '%s' "$all" | tr -d '[]",')
if [ "$(printf '%s\n' $groups | sort)" != "$(printf '%s\n' $listed | sort)" ]; then
    echo "error: ci/check.sh has groups $groups but ci/ci_groups.sh runs $listed" >&2
    fail=1
fi

exit "$fail"
