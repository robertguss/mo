#!/bin/sh
# Tests ci/library_map.sh on trees of its own. The check must pass a tree whose map and tables
# agree, and fail each tree that breaks one rule, naming the file and line that break it and
# why.
#
#   sh ci/test_library_map.sh
set -eu
check="$(cd "$(dirname "$0")" && pwd)/library_map.sh"
tree=$(mktemp -d)
trap 'rm -rf "$tree"' EXIT
trap 'exit 1' HUP INT TERM
failures=0

fail() {
	echo "test_library_map: $*" >&2
	failures=$((failures + 1))
}

# A tree whose map and tables agree. The map has a table the check doesn't read, rows of
# every place, an operator implemented by a primitive, a constant, a function called through
# its module, a lowering row that names primitives, and Mo rows in modules of one word and two.
good_tree() {
	rm -rf "$tree/docs" "$tree/src"
	mkdir -p "$tree/docs/design" "$tree/src/base"
	cat >"$tree/src/base/prims.def" <<'EOF'
/* PRIM(id, module, name, signature, params, bugs) */
#ifndef PRIM
#error "Define PRIM before including base/prims.def."
#endif
PRIM(INT_ABS,     "Int",    "abs",     "fn(Int) : Int",                "b",  "overflow")
PRIM(BIGINT_ADD,  "BigInt", "%add",    "fn(BigInt, BigInt) : BigInt",  "bb", "")
PRIM(LIST_EMPTY,  "List",   "%empty",  "List(T)",                      "",   "")
PRIM(LIST_GET,    "List",   "get",     "fn(List(T), Int) : Option(T)", "bb", "")
PRIM(SET_EMPTY,   "Set",    "empty",   "Set(T)",                       "",   "")
PRIM(TEXT_TRIM,   "Text",   "trim",    "fn(Text) : Text",              "o",  "")
PRIM(TEXT_FROM_CHARS, "Text", "from_chars", "fn(List(Char)) : Text",   "b",  "")
#undef PRIM
EOF
	cat >"$tree/src/base/hostops.def" <<'EOF'
HOSTOP(CONSOLE_PRINT, "Console", "print", "fn(Console, Text)", "bb", "printing to an output that is gone", false)
HOSTOP(CONSOLE_FAKE,  "Console", "fake",  "fn() : Console",    "",   "",                                  true)
#undef HOSTOP
EOF
	cat >"$tree/docs/design/library.md" <<'EOF'
# A map

| Where | Implemented as |
| --- | --- |
| primitive | C, with `NOT_AN_ID_HERE` |

## Int

| Function | Where | Implemented by | Why |
| --- | --- | --- | --- |
| `a + b` | instruction | `add` at `int` | |
| `abs` | primitive | `INT_ABS` | |

## BigInt

| Function | Where | Implemented by | Why |
| --- | --- | --- | --- |
| `a + b` | primitive | `BIGINT_ADD` | a reference |
| `a == b` | generic | `equal` | |

## List(T)

| Function | Where | Implemented by | Why |
| --- | --- | --- | --- |
| `[a, b]` | lowering | `List.%empty`, then `append`: `LIST_EMPTY` | |
| `List.%empty` | primitive | `LIST_EMPTY` | |
| `get(i)` | primitive | `LIST_GET` | |
| `map(f)` | Mo | `lib/list.mo` | takes a function |

```text
| this | is | code | not | a | row |
```

## Set(T)

| Function | Where | Implemented by | Why |
| --- | --- | --- | --- |
| `Set.empty` | primitive | `SET_EMPTY` | a constant |

## Text

| Function | Where | Implemented by | Why |
| --- | --- | --- | --- |
| `trim` | primitive | `TEXT_TRIM` | |
| `Text.from_chars(chars)` | primitive | `TEXT_FROM_CHARS` | |

## Console

| Function | Where | Implemented by | Why |
| --- | --- | --- | --- |
| `print(text)` | host | `CONSOLE_PRINT` | |
| `Console.fake()` | host | `CONSOLE_FAKE` | |

## FileError

| Function | Where | Implemented by | Why |
| --- | --- | --- | --- |
| `to_text` | Mo | `lib/file-error.mo` | its own `to_text` |
EOF
}

# Runs the check on the tree. It must fail, with a line that starts with where and contains
# why.
expect_refusal() {
	where=$1 why=$2 status=0
	output=$(sh "$check" "$tree" 2>&1) || status=$?
	if [ "$status" -ne 1 ]; then
		fail "$where: expected the check to fail with exit status 1, not $status:" "$output"
		return
	fi
	if ! printf '%s\n' "$output" | awk -v where="$where" -v why="$why" \
		'index($0, where) == 1 && index($0, why) > 0 { found = 1 } END { exit !found }'; then
		fail "$where: expected a refusal that says \"$why\", not:" "$output"
	fi
}

# Replaces a line of a file of the tree that agrees, given by its text, and expects the check
# to refuse the tree at the given line of the given file.
refuses() {
	file=$1 old=$2 new=$3 refused=$4 why=$5
	good_tree
	if ! grep -qxF -- "$old" "$tree/$file"; then
		fail "the good tree has no line \"$old\" in $file"
		return
	fi
	awk -v old="$old" -v new="$new" '$0 == old { if (new != "") print new; next } { print }' \
		"$tree/$file" >"$tree/replaced" && mv "$tree/replaced" "$tree/$file"
	expect_refusal "$tree/$refused: " "$why"
}

map=docs/design/library.md
prims=src/base/prims.def
hostops=src/base/hostops.def

good_tree
status=0
output=$(sh "$check" "$tree" 2>&1) || status=$?
if [ "$status" -ne 0 ]; then
	fail "the check refused a tree whose map and tables agree:" "$output"
fi
case $output in
*"each of the 7 primitives and 2 host operations (14 rows)"*) ;;
*) fail "the check passed the good tree but counted wrongly:" "$output" ;;
esac

refuses $map '| `abs` | primitive | `INT_ABS` | |' '| `abs` | primitive | `INT_ABSOLUTE` | |' \
	"$map:12" 'INT_ABSOLUTE is no line of'
refuses $map '| `abs` | primitive | `INT_ABS` | |' '' "$prims:5" 'INT_ABS has no primitive row'
refuses $map '| `print(text)` | host | `CONSOLE_PRINT` | |' '| `print(text)` | host | `LIST_GET` | |' \
	"$map:51" 'a host row names LIST_GET, which is a line of'
refuses $map '| `abs` | primitive | `INT_ABS` | |' '| `abs` | primitive, in C | `INT_ABS` | |' \
	"$map:12" "isn't instruction, primitive, host, generic, lowering or Mo"
refuses $map '| `abs` | primitive | `INT_ABS` | |' '| `abs` | primitive | `INT_ABS`, or `BIGINT_ADD` | |' \
	"$map:12" 'names 2 ids in backticks, not exactly one'
refuses $map '| `abs` | primitive | `INT_ABS` | |' '| `abs` | primitive | the absolute value | |' \
	"$map:12" 'names 0 ids in backticks, not exactly one'
refuses $map '| `abs` | primitive | `INT_ABS` | |' '| `abs` | primitive | `INT_ABS` |' \
	"$map:12" 'a row needs four columns'
refuses $map '| `trim` | primitive | `TEXT_TRIM` | |' '| `trim` | primitive | `INT_ABS` | |' \
	"$map:44" 'INT_ABS is in module Int, not Text'
refuses $map '| `trim` | primitive | `TEXT_TRIM` | |' '| `strip` | primitive | `TEXT_TRIM` | |' \
	"$map:44" 'the row is about strip, but TEXT_TRIM is named trim'
refuses $map '| `Text.from_chars(chars)` | primitive | `TEXT_FROM_CHARS` | |' \
	'| `List.from_chars(chars)` | primitive | `TEXT_FROM_CHARS` | |' \
	"$map:45" 'the row is about List.from_chars, but TEXT_FROM_CHARS is in module Text'
refuses $map '| `get(i)` | primitive | `LIST_GET` | |' '| `get(i)` | primitive | `LIST_EMPTY` | |' \
	"$map:27" 'LIST_EMPTY has a row already'
refuses $map '| `[a, b]` | lowering | `List.%empty`, then `append`: `LIST_EMPTY` | |' \
	'| `[a, b]` | lowering | `List.%empty`, then `LIST_APPEND` | |' \
	"$map:25" 'LIST_APPEND is no line of'
refuses $hostops 'HOSTOP(CONSOLE_FAKE,  "Console", "fake",  "fn() : Console",    "",   "",                                  true)' \
	'' "$map:52" 'CONSOLE_FAKE is no line of'
refuses $prims 'PRIM(INT_ABS,     "Int",    "abs",     "fn(Int) : Int",                "b",  "overflow")' \
	'PRIM(INT_ABS)' "$prims:5" 'a line needs an id, a module and a name'
refuses $prims 'PRIM(LIST_GET,    "List",   "get",     "fn(List(T), Int) : Option(T)", "bb", "")' \
	'PRIM(INT_ABS,     "Int",    "abs",     "fn(Int) : Int",                "b",  "overflow")' \
	"$prims:8" 'INT_ABS is a line of both tables, or twice of one'

refuses $map '| `map(f)` | Mo | `lib/list.mo` | takes a function |' \
	'| `map(f)` | Mo | `lib/lists.mo` | takes a function |' \
	"$map:28" 'a Mo row of section "List(T)" names lib/lists.mo, not lib/list.mo'
refuses $map '| `to_text` | Mo | `lib/file-error.mo` | its own `to_text` |' \
	'| `to_text` | Mo | `lib/fileerror.mo` | its own `to_text` |' \
	"$map:58" 'names lib/fileerror.mo, not lib/file-error.mo'
refuses $map '| `map(f)` | Mo | `lib/list.mo` | takes a function |' \
	'| `map(f)` | Mo | Mo code | takes a function |' \
	"$map:28" 'a Mo row names its file in lib/ in backticks, as lib/list.mo'
refuses $map '| `map(f)` | Mo | `lib/list.mo` | takes a function |' \
	'| `get(i)` | Mo | `lib/list.mo` | takes a function |' \
	"$map:28" 'get has a row already in section "List(T)"'

good_tree
rm "$tree/$hostops"
status=0
output=$(sh "$check" "$tree" 2>&1) || status=$?
if [ "$status" -ne 2 ]; then
	fail "expected the check to fail with exit status 2 without hostops.def, not $status:" "$output"
fi

if [ "$failures" -ne 0 ]; then
	echo "test_library_map: $failures check(s) failed" >&2
	exit 1
fi
echo "test_library_map: all checks passed"
