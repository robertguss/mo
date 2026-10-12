#!/bin/sh
# Exercise the actual file/CLI protocol, including distinct diagnostic exits.
set -eu
bin="$(pwd)/formal/.lake/build/bin/mocore"
tree=$(mktemp -d)
trap 'rm -rf "$tree"' EXIT
trap 'exit 1' HUP INT TERM

world='(World () () () () () () 0 0)'
program='(Program () ((FunDecl "main" () (int) (lit (int 7)) "Main" (function) 0)) () "main")'
printf '(core 0.1 %s)\n' "$world" >"$tree/world"
printf '(core 0.1 %s)\n' "$program" >"$tree/program"
printf '(core 0.1 (Run %s %s (entry ()) 30))\n' "$program" "$world" >"$tree/run"

check() {
	expected_status=$1 expected_text=$2
	shift 2
	status=0
	actual=$("$bin" "$@") || status=$?
	if [ "$status" != "$expected_status" ] || [ "$actual" != "$expected_text" ]; then
		printf 'mocore: expected exit %s, got %s\nexpected: %s\nactual: %s\n' \
			"$expected_status" "$status" "$expected_text" "$actual" >&2
		exit 1
	fi
}

check 0 '(core 0.1 (returned "7" (effects () () ())))' "$tree/run"
check 0 '(core 0.1 (returned "7" (effects () () ())))' "$tree/program" "$tree/world"
check 3 '(core 0.1 (outOfFuel))' "$tree/program" "$tree/world" --fuel 0
check 6 '(core 0.1 (invalidRun "test not found: missing"))' "$tree/program" "$tree/world" --test missing

bad='(Program () ((FunDecl "main" () (int) (lit (text "wrong")) "Main" (function) 0)) () "main")'
printf '(core 0.1 %s)\n' "$bad" >"$tree/bad"
status=0
actual=$("$bin" "$tree/bad" "$tree/world") || status=$?
[ "$status" = 5 ]
case "$actual" in '(core 0.1 (illTyped '*')' ) ;; *) echo "mocore: missing illTyped report" >&2; exit 1 ;; esac
printf '(core 0.1 (Run %s %s (entry ()) 30))\n' "$bad" "$world" >"$tree/bad-run"
status=0
actual=$("$bin" "$tree/bad-run") || status=$?
[ "$status" = 5 ]

printf '(core 9.9 (unit))\n' >"$tree/parse"
check 4 '(core 0.1 (parseError "expected (core 0.1 payload)"))' "$tree/parse"
printf '\377' >"$tree/utf8"
check 4 '(core 0.1 (parseError "input is not valid UTF-8"))' "$tree/utf8"

spec='(Program () ((FunDecl "main" () (unit) (lit (unit)) "Main" (function) 0) (FunDecl "failure" () (unit) (fail (lit (text "detail"))) "Spec" (test) 0) (FunDecl "example" () (unit) (lit (unit)) "Spec" (example) 0)) () "main")'
printf '(core 0.1 %s)\n' "$spec" >"$tree/spec"
check 2 '(core 0.1 (failed "detail" (effects () () ())))' "$tree/spec" "$tree/world" --test failure
check 0 '(core 0.1 (returned "Unit" (effects () () ())))' "$tree/spec" "$tree/world" --test example

stopped='(Program () ((FunDecl "main" (("c" (cap (console)))) (unit) (let_ "_" (prim (io (print)) ((var "c") (lit (text "before")))) (stop "bug")) "Main" (function) 0)) () "main")'
printf '(core 0.1 %s)\n' "$stopped" >"$tree/stopped"
check 1 '(core 0.1 (stopped (stop "bug") (effects ("before") () ())))' "$tree/stopped" "$tree/world"

echo 'mocore: file protocol, returned/stopped/failed, checking, UTF-8, selection and fuel passed'
