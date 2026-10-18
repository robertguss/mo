#!/usr/bin/env bash
# Runs the CBMC proofs in cbmc/reader.c (make cbmc). Each proof checks one function of
# src/bytecode/read.c against its specification, with the functions it calls replaced by their
# models (cbmc/README.md).
#
#   bash cbmc/prove.sh [proof...]           with no names, every proof
#   bash cbmc/prove.sh --reach [proof...]   check instead that each harness reaches its end
#
# MO_CBMC_INPUT (default 32) is the largest input, in bytes, the proofs cover. Logs and goto
# programs go to build/cbmc/.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

input=${MO_CBMC_INPUT:-32}
build=build/cbmc
mkdir -p "$build"
reach=
if [[ "${1:-}" == --reach ]]; then
	reach=-DMO_CBMC_REACH
	shift
fi

# name | functions replaced by their models (model_<name>) | loop bounds
entries=$((input / 4 + 1)) # a table has at most this many entries, plus one for the loop's exit
proofs=(
	"word||"
	"boolean|word|"
	"allocate||"
	"scratch||"
	"array|word allocate|"
	"indices|array word|indices.0:$((input / 4 + 1))"
	"utf8||mo_bytecode_utf8.0:4,mo_bytecode_utf8.1:$((input + 1))"
	"blob|word allocate mo_bytecode_utf8:model_utf8|"
	"name|word|"
	"fields|array name word|fields.0:$((input / 8 + 1))"
	"choice_cases|name array fields|choice_cases.0:$((input / 8 + 1))"
	"type_entry|indices word name fields choice_cases|"
	"types|array word type_entry|types.0:$((input / 4 + 1))"
	"constants|array word blob|constants.0:$((input / 8 + 1))"
	"parameters|array word|parameters.0:$((input / 8 + 1))"
	"functions|array scratch word boolean parameters indices|functions.0:$((input / 36 + 1))"
	"module_entry|word array boolean indices|module_entry.0:$((input / 8 + 1))"
	"debug_entry|array word|debug_entry.0:$((input / 16 + 1)),debug_entry.1:$((input / 16 + 1)),debug_entry.2:$((input / 16 + 1))"
	"sections|array blob types constants functions indices word module_entry debug_entry scratch|sections.0:$((input / 4 + 1)),sections.1:$((input / 12 + 1)),sections.2:$((input / 12 + 1)),sections.3:$((input / 16 + 1)),sections.4:$((input / 5 + 1)),sections.5:$((input / 36 + 1)),memchr.0:$((input + 1)),mo_bytecode_names_init.0:$((input / 4 + 1))"
	"decode|allocate sections|mo_bytecode_decode_info.0:9,mo_bytecode_decode_info.1:$((input + 1)),mo_bytecode_decode_info.2:$((input / 4 + 1)),mo_bytecode_decode_info.3:$((input / 4 + 1)),memcmp.0:5"
)

checks=(--bounds-check --pointer-check --pointer-overflow-check --signed-overflow-check
	--conversion-check --undefined-shift-check --div-by-zero-check --float-overflow-check
	--nan-check --pointer-primitive-check)

wanted=" $* "
failed=()
for proof in "${proofs[@]}"; do
	IFS='|' read -r name replaced loops <<<"$proof"
	if [[ -n "$*" && "$wanted" != *" $name "* ]]; then continue; fi
	harness=harness_${name}
	entry=$build/$name${reach:+.reach}.entry.goto
	instrumented=$build/$name${reach:+.reach}.goto
	log=$build/$name${reach:+.reach}.log
	if ! goto-cc -std=c17 -Isrc -DMO_CBMC_INPUT="$input" $reach --function "$harness" cbmc/reader.c \
		-o "$entry" >"$log" 2>&1; then
		echo "FAILED $name: cbmc/reader.c does not compile; see $log"
		failed+=("$name")
		continue
	fi
	replace=()
	for callee in $replaced; do
		if [[ "$callee" == *:* ]]; then replace+=(--replace-calls "$callee");
		else replace+=(--replace-calls "$callee:model_$callee"); fi
	done
	cp "$entry" "$instrumented"
	if ((${#replace[@]})) && ! goto-instrument "${replace[@]}" "$entry" "$instrumented" >>"$log" 2>&1; then
		echo "FAILED $name: could not replace its callees by their models; see $log"
		failed+=("$name")
		continue
	fi
	# Every loop bound carries an unwinding assertion, so a bound too small fails the proof
	# rather than hiding paths.
	common="any_input.0:$((input + 1)),word.0:5,memcpy.0:$((input + 1)),cases_bounded.0:$entries,model_types.0:$entries,model_sections.0:$entries"
	start=$SECONDS
	if [[ -n "$reach" ]]; then
		# The only failure must be the end-of-harness assertion.
		cbmc "$instrumented" --unwind 1 --unwindset "$common${loops:+,$loops}" >>"$log" 2>&1 || true
		if grep -q 'the harness reaches its end: FAILURE$' "$log" &&
			[[ $(grep -c ': FAILURE$' "$log") == 1 ]]; then
			echo "reached the end of $name ($((SECONDS - start))s)"
		else
			echo "FAILED $name: no path reaches the end of harness_$name; see $log"
			failed+=("$name")
		fi
		continue
	fi
	if cbmc "$instrumented" "${checks[@]}" --unwind 1 --unwinding-assertions \
		--unwindset "$common${loops:+,$loops}" >>"$log" 2>&1; then
		echo "proved $name ($((SECONDS - start))s)"
	else
		echo "FAILED $name: see $log"
		grep -E ': FAILURE$|ERROR$|^error' "$log" | head -n 20 || true
		failed+=("$name")
	fi
done
if ((${#failed[@]})); then
	echo "error: ${#failed[@]} CBMC proof(s) failed: ${failed[*]}" >&2
	exit 1
fi
