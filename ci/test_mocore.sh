#!/bin/sh
# Exercise the actual file/CLI protocol, including distinct diagnostic exits.
set -eu
bin="$(pwd)/formal/.lake/build/bin/mocore"
tree=$(mktemp -d)
trap 'rm -rf "$tree"' EXIT
trap 'exit 1' HUP INT TERM

random='(RandomState 0xe220a8397b1dcdaf 0x6e789e6aa1b965f4 0x06c45d188009454f 0xf88bb8a8724c81ec)'
world="(World () () () () () () 0 $random ())"
program='(Program () ((FunDecl "main" () (int) (lit (int 7)) "Main" (function) 0)) () "main" (noSource))'
printf '(core 0.5 %s)\n' "$world" >"$tree/world"
printf '(core 0.5 %s)\n' "$program" >"$tree/program"
printf '(core 0.5 (Run %s %s (entry ()) 30))\n' "$program" "$world" >"$tree/run"

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

check 0 '(core 0.5 (returned "7" (effects () () () ())))' "$tree/run"
check 0 '(core 0.5 (returned "7" (effects () () () ())))' "$tree/program" "$tree/world"
check 3 '(core 0.5 (outOfFuel))' "$tree/program" "$tree/world" --fuel 0
check 6 '(core 0.5 (invalidRun "test not found: missing"))' "$tree/program" "$tree/world" --test missing

bad='(Program () ((FunDecl "main" () (int) (lit (text "wrong")) "Main" (function) 0)) () "main" (noSource))'
printf '(core 0.5 %s)\n' "$bad" >"$tree/bad"
status=0
actual=$("$bin" "$tree/bad" "$tree/world") || status=$?
[ "$status" = 5 ]
case "$actual" in '(core 0.5 (illTyped '*')' ) ;; *) echo "mocore: missing illTyped report" >&2; exit 1 ;; esac
printf '(core 0.5 (Run %s %s (entry ()) 30))\n' "$bad" "$world" >"$tree/bad-run"
status=0
actual=$("$bin" "$tree/bad-run") || status=$?
[ "$status" = 5 ]

printf '(core 9.9 (unit))\n' >"$tree/parse"
check 4 '(core 0.5 (parseError "expected (core 0.5 payload)"))' "$tree/parse"
for version in 0.1 0.2 0.3 0.4 0.6; do
	printf '(core %s (unit))\n' "$version" >"$tree/old"
	check 4 '(core 0.5 (parseError "expected (core 0.5 payload)"))' "$tree/old"
done
printf '\377' >"$tree/utf8"
check 4 '(core 0.5 (parseError "input is not valid UTF-8"))' "$tree/utf8"

spec='(Program () ((FunDecl "main" () (unit) (lit (unit)) "Main" (function) 0) (FunDecl "failure" () (unit) (fail (lit (text "detail"))) "Spec" (test) 0) (FunDecl "example" () (unit) (lit (unit)) "Spec" (example) 0)) () "main" (noSource))'
printf '(core 0.5 %s)\n' "$spec" >"$tree/spec"
check 2 '(core 0.5 (failed "detail" (effects () () () ())))' "$tree/spec" "$tree/world" --test failure
check 0 '(core 0.5 (returned "Unit" (effects () () () ())))' "$tree/spec" "$tree/world" --test example

stopped='(Program () ((FunDecl "main" (("c" (cap (console)))) (unit) (let_ "_" (prim (io (print)) ((var "c") (lit (text "before")))) (stop "bug")) "Main" (function) 0)) () "main" (noSource))'
printf '(core 0.5 %s)\n' "$stopped" >"$tree/stopped"
check 1 '(core 0.5 (stopped (stop "bug") (effects ("before") () () ())))' "$tree/stopped" "$tree/world"

# A spec constructs a fake and passes it to an ordinary function. Every
# terminal outcome retains the fake's output without leaking into real stdout.
fake='(Program () ((FunDecl "main" (("c" (cap (console)))) (unit) (prim (io (print)) ((var "c") (lit (text "hello")))) "Main" (function) 0) (FunDecl "fake" () (unit) (let_ "c" (prim (io (consoleFake)) ()) (let_ "_" (call "main" ((var "c"))) %s)) "Spec" (test) 0)) () "main" (noSource))'
fake_effects="(effects () () () ((CapState () () () (\"hello\") () () 0 $random)))"
for ending in returned stopped failed; do
	case "$ending" in
		returned) tail='(lit (unit))'; status=0; report='returned "Unit"' ;;
		stopped) tail='(stop "bug")'; status=1; report='stopped (stop "bug")' ;;
		failed) tail='(fail (lit (text "detail")))'; status=2; report='failed "detail"' ;;
	esac
	# The format is fixed above; only the final expression is substituted.
	printf "$fake\n" "$tail" >"$tree/fake-payload"
	printf '(core 0.5 (Run %s %s (test "fake") 50))\n' "$(cat "$tree/fake-payload")" "$world" >"$tree/fake"
	check "$status" "(core 0.5 ($report $fake_effects))" "$tree/fake"
done

# Trusted Unicode has no substitute implementation in the oracle. All five
# reached calls stop with prior effects retained; an unchosen call does not.
for op in graphemes displayWidth normalizeNfc lowercase uppercase; do
	case "$op" in
		graphemes) ty='(list (text))' ;;
		displayWidth) ty='(int)' ;;
		*) ty='(text)' ;;
	esac
	unicode="(Program () ((FunDecl \"main\" ((\"c\" (cap (console)))) $ty (let_ \"_\" (prim (io (print)) ((var \"c\") (lit (text \"before unicode\")))) (prim (unicode16 ($op)) ((lit (text \"éß👩‍💻\"))))) \"Main\" (function) 0)) () \"main\" (noSource))"
	printf '(core 0.5 %s)\n' "$unicode" >"$tree/unicode"
	check 1 '(core 0.5 (stopped (outside "Unicode16 is a trusted primitive unavailable in mocore") (effects ("before unicode") () () ())))' "$tree/unicode" "$tree/world"
done
skipped='(Program () ((FunDecl "main" () (int) (if_ (lit (bool false)) (prim (unicode16 (displayWidth)) ((lit (text "ignored")))) (lit (int 7))) "Main" (function) 0)) () "main" (noSource))'
printf '(core 0.5 %s)\n' "$skipped" >"$tree/skipped"
check 0 '(core 0.5 (returned "7" (effects () () () ())))' "$tree/skipped" "$tree/world"

# Native regression: round the Decimal's exact ratio, not its already-rounded count.
decimal='(Program () ((FunDecl "main" () (float) (prim (dec (toFloat)) ((lit (decimal 1000000000000000065)))) "Main" (function) 0)) () "main" (noSource))'
printf '(core 0.5 %s)\n' "$decimal" >"$tree/decimal"
check 0 '(core 0.5 (returned "0x3ff0000000000000" (effects () () () ())))' "$tree/decimal" "$tree/world"

# Compiler metadata survives decoding but cannot change the oracle's report.
check 0 '(core 0.5 (returned "12" (effects () () () ())))' tests/core/Metadata.core
for change in 's/SourceSpan 0 0 40/SourceSpan 1 7 4294967295/' \
	's/a sentence with β/a different sentence with γ/' 's/((0 false))/((0 true))/'; do
	sed "$change" tests/core/Metadata.core >"$tree/metadata"
	check 0 '(core 0.5 (returned "12" (effects () () () ())))' "$tree/metadata"
done
sed 's/100))$/0))/' tests/core/Metadata.core >"$tree/metadata"
check 3 '(core 0.5 (outOfFuel))' "$tree/metadata"
sed 's/SourceSpan 0 0 40/SourceSpan 2 0 40/' tests/core/Metadata.core >"$tree/metadata"
check 4 '(core 0.5 (parseError "invalid compiler metadata shape"))' "$tree/metadata"

echo 'mocore: file protocol, real/fake effects, returned/stopped/failed, checking, UTF-8, selection, fuel, single rounding, metadata independence and trusted Unicode boundaries passed'
