#!/bin/sh
# Tests ci/deps.sh on trees of its own. The check must pass a tree that follows every rule, and
# fail each tree that breaks one rule once, naming the file and line that break it and why.
#
#   sh ci/test_deps.sh
set -eu
deps="$(cd "$(dirname "$0")" && pwd)/deps.sh"
tree=$(mktemp -d)
trap 'rm -rf "$tree"' EXIT
trap 'exit 1' HUP INT TERM
failures=0

fail() {
	echo "test_deps: $*" >&2
	failures=$((failures + 1))
}

# Writes a file of the tree's src/ with the lines given.
write() {
	file=$tree/src/$1
	shift
	mkdir -p "$(dirname "$file")"
	printf '%s\n' "$@" >"$file"
}

# A tree that follows every rule. Each module includes base, its own files, and the public
# headers of the modules it uses; codegen and bytecode use only heap/types.h of heap. A header
# in angle brackets that doesn't start with a module is a system header, and a file that isn't
# C is not read.
good_tree() {
	rm -rf "$tree/src"
	write base/base.h '#include <limits.h>' '#include "base/limits.h"'
	write base/limits.h ''
	write base/prims.def '#include "base/base.h"'
	write base/base.c '#include "base/base.h"'
	write syntax/syntax.h '#include "base/base.h"'
	write syntax/lexer.h '#include "base/limits.h"'
	write syntax/parser.c '#include "syntax/lexer.h"' '#include "syntax/syntax.h"' '#include <stdio.h>'
	write check/check.h '#include "syntax/syntax.h"'
	write check/names.c '  #  include "check/check.h"' '#include <sys/types.h>'
	write lower/lower.h '#include "core/core.h"'
	write lower/lower.c '#include "check/check.h"' '#include "lower/lower.h"'
	write core/core.h '#include "base/base.h"'
	write codegen/codegen.h '#include "core/core.h"'
	write codegen/codegen.c '#include "bytecode/bytecode.h"' '#include "codegen/codegen.h"' \
		'#include "heap/types.h"'
	write bytecode/bytecode.h '#include "heap/types.h"'
	write heap/types.h '#include "base/base.h"'
	write heap/heap.h '#include "heap/types.h"'
	write prim/prim.h '#include "heap/heap.h"'
	write host/host.h '#include "heap/heap.h"' '#include "heap/types.h"'
	write vm/vm.h '#include "bytecode/bytecode.h"'
	write vm/vm.c '#include "base/prims.def"' '#include "heap/heap.h"' '#include "host/host.h"' \
		'#include "prim/prim.h"' '#include "vm/vm.h"'
	write tools/cli.h ''
	write tools/main.c '#include "base/base.h"' '#include "bytecode/bytecode.h"' \
		'#include "check/check.h"' '#include "codegen/codegen.h"' '#include "core/core.h"' \
		'#include "heap/heap.h"' '#include "heap/types.h"' '#include "host/host.h"' \
		'#include "lower/lower.h"' '#include "prim/prim.h"' '#include "syntax/syntax.h"' \
		'#include "tools/cli.h"' '#include "vm/vm.h"'
	write README.md '#include "vm/vm.h"'
}

# Runs the check on the tree. It must fail, with a line that starts with where and contains
# why.
expect_refusal() {
	where=$1 why=$2 status=0
	output=$(cd "$tree" && sh "$deps" src 2>&1) || status=$?
	if [ "$status" -ne 1 ]; then
		fail "$where: expected the check to fail with exit status 1, not $status:" "$output"
		return
	fi
	if ! printf '%s\n' "$output" | awk -v where="$where" -v why="$why" \
		'index($0, where) == 1 && index($0, why) > 0 { found = 1 } END { exit !found }'; then
		fail "$where: expected a refusal that says \"$why\", not:" "$output"
	fi
}

# Adds an include to the end of a file of the tree that follows every rule, and expects the
# check to refuse it.
refuses() {
	good_tree
	printf '%s\n' "$2" >>"$tree/src/$1"
	expect_refusal "src/$1:$(($(wc -l <"$tree/src/$1"))): " "$3"
}

good_tree
status=0
output=$(cd "$tree" && sh "$deps" src 2>&1) || status=$?
if [ "$status" -ne 0 ]; then
	fail "the check refused a tree that follows every rule:" "$output"
fi

refuses syntax/parser.c '#include "check/check.h"' 'syntax does not use check'
refuses base/base.h '#include "heap/types.h"' 'base does not use heap'
refuses vm/vm.c ' # include "core/core.h"' 'vm does not use core'
refuses check/names.c '#include "syntax/lexer.h"' 'private to syntax'
refuses bytecode/bytecode.h '#include "heap/heap.h"' 'bytecode uses only heap/types.h of heap'
refuses codegen/codegen.c '#include "heap/heap.h"' 'codegen uses only heap/types.h of heap'
refuses tools/main.c '#include "base/base.c"' 'private to base'
refuses base/prims.def '#include "vm/vm.h"' 'base does not use vm'
refuses heap/heap.h '#include <prim/prim.h>' 'heap does not use prim'
refuses syntax/parser.c '#include "lexer.h"' 'not a module path'
refuses syntax/parser.c '#include "../vm/vm.h"' 'not a module path'
refuses tools/main.c '#include "util/util.h"' 'util is not a module'

good_tree
write util/util.c '#include "base/base.h"'
expect_refusal "src/util/util.c: " 'util is not a module'

good_tree
write main.c '#include "base/base.h"'
expect_refusal "src/main.c: " 'not in the directory of a module'

if [ "$failures" -ne 0 ]; then
	echo "test_deps: $failures check(s) failed" >&2
	exit 1
fi
echo "test_deps: all checks passed"
