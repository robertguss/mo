#!/usr/bin/env bash
# Runs the CBMC proofs in cbmc/reader.c, cbmc/arena.c and cbmc/verify.c (make cbmc). Each proof
# checks one function of src/bytecode/read.c, src/base/arena.c or the bytecode verifier against
# its specification, with the functions it calls replaced by their models (cbmc/README.md).
#
#   bash cbmc/prove.sh [proof...]           with no names, every proof
#   bash cbmc/prove.sh --reach [proof...]   check instead that every block of each proved
#                                           function, and the end of its harness, runs on
#                                           some input
#
# Each proof covers every input of up to MO_CBMC_INPUT bytes (default 32), or more where the
# proof needs more to run each loop's body. MO_CBMC_JOBS (default 1) proofs run at once. Logs and
# goto programs go to build/cbmc/.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

default_input=${MO_CBMC_INPUT:-32}
build=build/cbmc
mkdir -p "$build"
reach=
if [[ "${1:-}" == --reach ]]; then
	reach=1
	shift
fi

# name | functions replaced by their models (model_<name>) | loop bounds | least input size |
# file in cbmc/ (default reader.c) | more cbmc options
#
# A loop bound is written for an input of n bytes: "f.k:/d" is n / d + 1 (a loop over entries of
# at least d bytes, plus one for the loop's exit), "f.k:+" is n + 1 (a loop over bytes) and
# "f.k:c" is c. The least input size lets every loop's body run.
proofs=(
	"word|||"
	"boolean|word||"
	"allocate|||"
	"scratch|||"
	"array|word allocate||"
	"indices|array word|indices.0:/4|"
	"utf8||mo_bytecode_utf8.0:4,mo_bytecode_utf8.1:+|"
	"blob|word allocate mo_bytecode_utf8:model_utf8||"
	"name|word||"
	"fields|array name word|fields.0:/8|"
	"choice_cases|name array fields|choice_cases.0:/8|"
	"type_entry|indices word name fields choice_cases||"
	"types|array word type_entry|types.0:/4|"
	"constants|array word blob|constants.0:/8|"
	"parameters|array word|parameters.0:/8|"
	"functions|array scratch word boolean parameters indices|functions.0:/36|40"
	"layout_entry|word indices||"
	"module_entry|word array boolean indices|module_entry.0:/8|"
	"file_entry|blob|memchr.0:+|"
	"debug_entry|array word|debug_entry.0:/16,debug_entry.1:/16,debug_entry.2:/16|52"
	"sections|array blob types layout_entry constants word indices functions module_entry file_entry debug_entry scratch|sections.0:/4,sections.1:/12,sections.2:/12,sections.3:/16,sections.4:/5,sections.5:/36,mo_bytecode_names_init.0:/4|64"
	"decode|allocate sections|mo_bytecode_decode_info.0:9,mo_bytecode_decode_info.1:+,mo_bytecode_decode_info.2:/4,mo_bytecode_decode_info.3:/4,memcmp.0:5|"
	"arena_allocate||||arena.c"
	"arena_allocate_array|mo_arena_allocate|||arena.c"
	"arena_resize||||arena.c"
	"arena_copy||||arena.c"
	"arena_copy_text||strlen.0:9||arena.c"
	"arena_free||harness_arena_free.0:4,mo_arena_free.0:4||arena.c|--memory-leak-check"
	"instruction||||verify.c|--object-bits 10 --unwind 10"
	"lookup||||verify.c|--unwind 10"
	"step||||verify.c|--object-bits 10 --unwind 10"
)

checks=(--bounds-check --pointer-check --pointer-overflow-check --signed-overflow-check
	--conversion-check --undefined-shift-check --div-by-zero-check --float-overflow-check
	--nan-check --pointer-primitive-check)

# The function a proof is about, when it is not the proof's own name.
proved_function() {
	case "$1" in
	utf8) echo mo_bytecode_utf8 ;;
	decode) echo mo_bytecode_decode_info ;;
	# An arena proof covers the static helpers its function calls, too.
	arena_allocate) echo 'mo_arena_allocate|add_chunk|fits|padding_for' ;;
	arena_resize) echo 'mo_arena_resize|grow_alone' ;;
	arena_free) echo 'mo_arena_free|mo_arena_init' ;;
	arena_*) echo "mo_$1" ;;
	# A verifier proof covers the helpers its function calls, including
	# mo_verify_closure_signature, which the validator shares (CBMC does not build the validator).
	instruction) echo 'mo_verify_instruction|reg|kind|mo_verify_closure_signature|field|args' ;;
	lookup) echo 'mo_verify_layout_lookup|layout_order' ;;
	step) echo 'step|access|mo_verify_parameter_modes|refuse|charge' ;;
	*) echo "$1" ;;
	esac
}

# Runs one proof (or, with --reach, its reach check), printing one line and any failures.
run() {
	local name replaced loops least file flags
	IFS='|' read -r name replaced loops least file flags <<<"$1"
	local source=cbmc/${file:-reader.c}
	local n=$default_input
	if [[ -n "$least" ]] && ((least > n)); then n=$least; fi
	# Only the reader's proofs are about inputs of n bytes.
	local inputs=
	if [[ -z "$file" ]]; then inputs=", $n bytes"; fi
	harness=harness_${name}
	suffix=${reach:+.reach}
	entry=$build/$name$suffix.entry.goto
	instrumented=$build/$name$suffix.goto
	log=$build/$name$suffix.log
	: >"$log"

	# The proof's own loop bounds, each at least 2 so that the loop's body can run once.
	bounds=
	short=
	IFS=',' read -r -a parts <<<"$loops"
	for part in "${parts[@]}"; do
		loop=${part%%:*}
		spec=${part#*:}
		case "$spec" in
		/*) bound=$((n / ${spec#/} + 1)) ;;
		+) bound=$((n + 1)) ;;
		*) bound=$spec ;;
		esac
		if ((bound < 2)); then short+=" $loop"; fi
		bounds+=",$loop:$bound"
	done
	if [[ -n "$short" ]]; then
		echo "FAILED $name: on $n-byte inputs the body of$short can never run; raise its least input size"
		return 1
	fi

	# A proof's default loop bound (--unwind k, else 1) comes out of its cbmc options.
	local options=() unwind=1 next_is_unwind=
	for option in $flags; do
		if [[ -n "$next_is_unwind" ]]; then
			unwind=$option
			next_is_unwind=
		elif [[ "$option" == --unwind ]]; then
			next_is_unwind=1
		else
			options+=("$option")
		fi
	done
	if ! goto-cc -std=c17 -Isrc -DMO_CBMC_INPUT="$n" --function "$harness" "$source" \
		-o "$entry" >>"$log" 2>&1; then
		echo "FAILED $name: $source does not compile; see $log"
		return 1
	fi
	replace=()
	for callee in $replaced; do
		if [[ "$callee" == *:* ]]; then replace+=(--replace-calls "$callee");
		else replace+=(--replace-calls "$callee:model_$callee"); fi
	done
	cp "$entry" "$instrumented"
	if ((${#replace[@]})) && ! goto-instrument "${replace[@]}" "$entry" "$instrumented" >>"$log" 2>&1; then
		echo "FAILED $name: could not replace its callees by their models; see $log"
		return 1
	fi
	entries=$((n / 4 + 1))
	unwindset="any_input.0:$((n + 1)),word.0:5,memcpy.0:$((n + 1)),cases_bounded.0:$entries,model_types.0:$entries,model_sections.0:$entries$bounds"
	start=$SECONDS
	if [[ -n "$reach" ]]; then
		# Every block of the proved function must run on some input, so no assumption or loop
		# bound leaves part of it unchecked, and so must the end of the harness (reached_end).
		function=$(proved_function "$name")
		local cover=()
		for option in "${options[@]}"; do
			if [[ "$option" != --memory-leak-check ]]; then cover+=("$option"); fi
		done
		cbmc "$instrumented" --cover location "${cover[@]}" --unwind "$unwind" \
			--unwindset "$unwindset" >>"$log" 2>&1 || true
		missed=
		pointer_sites=
		while IFS= read -r goal; do
			# An MO_ASSERT's failure branch, or an MO_UNREACHABLE, must never run: the proof
			# shows that.
			[[ "$goal" =~ \ file\ ([^ ]+)\ line\ ([0-9]+)\  ]] &&
				sed -n "${BASH_REMATCH[2]}p" "${BASH_REMATCH[1]}" |
				grep -qE 'MO_ASSERT\(|MO_UNREACHABLE\(' && continue
			# Nor can a call through a function pointer reach a target the harness never gives
			# it: CBMC adds that branch when it resolves the pointer, and closes it with
			# assume(false). One such branch per line is let off, no more.
			if [[ "$goal" =~ \ file\ ([^ ]+)\ line\ ([0-9]+)\ function\ ([^ ]+)\  ]]; then
				site="${BASH_REMATCH[1]} line ${BASH_REMATCH[2]} function ${BASH_REMATCH[3]}"
				if [[ "$pointer_sites" != *"|$site|"* ]] &&
					grep -qF "at file $site thread 0: dereferenced function pointer must be" "$log"; then
					pointer_sites+="|$site|"
					continue
				fi
			fi
			missed+="$goal"$'\n'
		done < <(grep -E "^\[($function|reached_end)\.coverage\.[0-9]+\] .*: FAILED$" "$log" || true)
		if grep -qE "^\[($function)\.coverage\.[0-9]+\] .*: SATISFIED$" "$log" &&
			grep -qE "^\[reached_end\.coverage\.[0-9]+\] .*: SATISFIED$" "$log" && [[ -z "$missed" ]]; then
			echo "reached every block of $name ($((SECONDS - start))s$inputs)"
		else
			echo "FAILED $name: some blocks never run${inputs:+ on $n-byte inputs}; see $log"
			if [[ -n "$missed" ]]; then printf %s "$missed" | head -n 20; fi
			return 1
		fi
		return 0
	fi
	# Every loop bound carries an unwinding assertion, so a bound too small fails the proof
	# rather than hiding paths.
	if cbmc "$instrumented" "${checks[@]}" "${options[@]}" --unwind "$unwind" --unwinding-assertions \
		--unwindset "$unwindset" >>"$log" 2>&1; then
		echo "proved $name ($((SECONDS - start))s$inputs)"
	else
		echo "FAILED $name: see $log"
		grep -E ': FAILURE$|ERROR$|^error' "$log" | head -n 20 || true
		return 1
	fi
}

jobs=${MO_CBMC_JOBS:-1}
wanted=" $* "
names=()
for proof in "${proofs[@]}"; do
	name=${proof%%|*}
	if [[ -n "$*" && "$wanted" != *" $name "* ]]; then continue; fi
	names+=("$name")
done
failed=()
if ((jobs == 1)); then
	for proof in "${proofs[@]}"; do
		name=${proof%%|*}
		if [[ " ${names[*]} " != *" $name "* ]]; then continue; fi
		run "$proof" || failed+=("$name")
	done
else
	# In parallel, start with the last proofs, which are the slowest, and print in order.
	for ((i = ${#proofs[@]} - 1; i >= 0; i--)); do
		proof=${proofs[i]}
		name=${proof%%|*}
		if [[ " ${names[*]} " != *" $name "* ]]; then continue; fi
		while (($(jobs -rp | wc -l) >= jobs)); do wait -n || true; done
		(run "$proof" >"$build/$name${reach:+.reach}.out" 2>&1 && echo 0 || echo 1) \
			>"$build/$name${reach:+.reach}.status" &
	done
	wait
	for name in "${names[@]}"; do
		cat "$build/$name${reach:+.reach}.out"
		if [[ $(cat "$build/$name${reach:+.reach}.status") != 0 ]]; then failed+=("$name"); fi
	done
fi
if ((${#failed[@]})); then
	echo "error: ${#failed[@]} CBMC proof(s) failed: ${failed[*]}" >&2
	exit 1
fi
