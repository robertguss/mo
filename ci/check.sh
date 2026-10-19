#!/usr/bin/env bash
# Every check robertguss/mo's CI runs, in order. Run it before every push: the
# CI orb runs exactly this on each pull request it is sent (decisions D208, D214).
#
# Locally, a check whose tool is missing is skipped with a note, and the summary
# at the end counts what was skipped instead of claiming that all checks passed.
# In CI (CI=1) a missing tool is an error, so nothing is skipped silently.
#
# With no arguments every check runs. Arguments pick groups of checks, so CI can
# run the groups as parallel jobs; together the groups are every check:
#   gcc clang   build and run the unit tests with that compiler
#   sanitize    the unit tests under AddressSanitizer and UndefinedBehaviorSanitizer
#   fuzz        every fuzz target briefly
#   drivers     the checks that drive the test executables: goldens, Unicode, codegen
#   lean        Mo Core in Lean, and the checks that compare the test executables with it
#   static      grammar, module dependencies, library map, frozen tool tasks, formatting, clang-tidy
#   cbmc        the CBMC proofs of the bytecode reader (cbmc/README.md)
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

all_groups=" gcc clang sanitize fuzz drivers lean static cbmc "
groups="$*"
for group in $groups; do
	if [[ "$all_groups" != *" $group "* ]]; then
		echo "usage: bash ci/check.sh [group...], where a group is one of:$all_groups" >&2
		exit 64
	fi
done
want() { [[ -z "$groups" || " $groups " == *" $1 "* ]]; }

skipped=() # why each skipped check was skipped

step() { printf '\n==> %s\n' "$*"; }
have() { command -v "$1" >/dev/null 2>&1; }
missing() {
	if [[ "${CI:-}" == 1 ]]; then
		echo "error: $1" >&2
		exit 1
	fi
	echo "skipped: $1"
	skipped+=("$1")
}
has_libfuzzer() {
	local dir
	dir=$(mktemp -d)
	printf 'int LLVMFuzzerTestOneInput(const unsigned char *d, unsigned long n);\nint LLVMFuzzerTestOneInput(const unsigned char *d, unsigned long n) { (void)d; (void)n; return 0; }\n' >"$dir/probe.c"
	local ok=1
	clang -fsanitize=fuzzer "$dir/probe.c" -o "$dir/probe" >/dev/null 2>&1 || ok=0
	rm -rf "$dir"
	((ok == 1))
}

if want gcc; then
step "Build and test with gcc (warnings are errors)"
if have gcc; then make --no-print-directory test CC=gcc; else missing "gcc is not installed"; fi

fi

if want clang; then
step "Build and test with clang (warnings are errors)"
if have clang; then make --no-print-directory test CC=clang; else missing "clang is not installed"; fi
fi

if want sanitize; then

step "Tests under AddressSanitizer and UndefinedBehaviorSanitizer"
if have clang; then
	make --no-print-directory sanitize CC=clang
elif have gcc; then
	make --no-print-directory sanitize CC=gcc
else
	missing "no compiler with sanitizers is installed"
fi
fi

# Run alone, the drivers and lean groups build the executables they drive, as the
# gcc, clang and sanitize groups would have.
if [[ -n "$groups" ]] && { want drivers || want lean; }; then
	step "Build the test executables the checks below drive"
	if have gcc; then make --no-print-directory test-executables CC=gcc; fi
	if have clang; then
		make --no-print-directory test-executables CC=clang
		make --no-print-directory sanitize-executables CC=clang
	elif have gcc; then
		make --no-print-directory sanitize-executables CC=gcc
	fi
fi

if want drivers; then
step "The mo command: text/JSON goldens, argument matrix and JSON validity"
if have python3; then
	drivers=()
	for driver in build/{gcc,clang}/{debug,sanitize}/mo; do
		if [[ -x "$driver" ]]; then drivers+=("$driver"); fi
	done
	if ((${#drivers[@]})); then python3 ci/check_cli.py "${drivers[@]}";
	else missing "no mo command executable is built"; fi
else
	missing "python3 is not installed"
fi

step "The session-recording harness: a recorded session's log reads to tally's example table"
if have python3; then
	if ((${#drivers[@]})); then PYTHONDONTWRITEBYTECODE=1 python3 learnability/harness/test_session.py "${drivers[@]}";
	else missing "no mo command executable is built"; fi
else
	missing "python3 is not installed"
fi

step "Every error code has a test of its exact message"
if have python3; then
	drivers=()
	for driver in build/{gcc,clang}/{debug,sanitize}/mo; do
		if [[ -x "$driver" ]]; then drivers+=("$driver"); fi
	done
	if ((${#drivers[@]})); then python3 ci/check_errors.py "${drivers[@]}";
	else missing "no mo command executable is built"; fi
else
	missing "python3 is not installed"
fi

step "The golden corpus: whole programs match their expected output and exit status"
if have python3; then
	drivers=()
	for driver in build/{gcc,clang}/{debug,sanitize}/mo; do
		if [[ -x "$driver" ]]; then drivers+=("$driver"); fi
	done
	if ((${#drivers[@]})); then python3 ci/check_golden.py "${drivers[@]}";
	else missing "no mo command executable is built"; fi
else
	missing "python3 is not installed"
fi

step "Bytecode text: repeated layout and BigInt conversion stay within work ceilings"
if have python3; then
	drivers=()
	for driver in build/{gcc,clang}/{debug,sanitize}/tests/test_bytecode_text.test; do
		if [[ -x "$driver" ]]; then drivers+=("$driver"); fi
	done
	if ((${#drivers[@]})); then python3 ci/check_bytecode_text.py "${drivers[@]}";
	else missing "no bytecode text test executable is built"; fi
else
	missing "python3 is not installed"
fi

step "Unicode 16: pinned tables, UCD conformance and long-text work bounds"
if have python3; then
	python3 ci/generate_unicode16.py --check
	drivers=()
	for driver in build/{gcc,clang}/{debug,sanitize}/tests/test_unicode16.test; do
		if [[ -x "$driver" ]]; then drivers+=("$driver"); fi
	done
	if ((${#drivers[@]})); then python3 ci/check_unicode16.py "${drivers[@]}";
	else missing "no Unicode primitive test executable is built"; fi
else
	missing "python3 is not installed"
fi
fi

if want fuzz; then
step "Fuzz every target briefly"
if have clang && has_libfuzzer; then
	make --no-print-directory fuzz-smoke
else
	missing "clang with libFuzzer is not installed"
fi
fi

if want static; then
step "The executable grammar: its tests, the examples and the reference's code parse"
if have bison && (have clang || have gcc); then
	if have clang; then make --no-print-directory grammar CC=clang; else make --no-print-directory grammar CC=gcc; fi
else
	missing "bison or a C compiler is not installed"
fi
fi

if want lean; then
step "Mo Core in Lean: the model builds and every proof checks"
# elan lives in /opt/elan where .agents/setup puts it, since CI runs with a
# fresh HOME, or else in the usual ~/.elan.
for elan_home in "${ELAN_HOME:-}" /opt/elan "$HOME/.elan"; do
	if [[ -n "$elan_home" && -x "$elan_home/bin/lake" ]]; then
		export ELAN_HOME="$elan_home" PATH="$elan_home/bin:$PATH"
		break
	fi
done
if have lake; then
	make --no-print-directory formal
	step "Core text fixtures match the Lean reference printer"
	sh ci/check_core_fixtures.sh
	step "Core checker verdicts match Lean on fixtures, generated variants and fuzz corpora"
	if have python3; then
		drivers=()
		for driver in build/{gcc,clang}/{debug,sanitize}/tests/test_core_check.test; do
			if [[ -x "$driver" ]]; then drivers+=("$driver"); fi
		done
		if ((${#drivers[@]})); then python3 ci/check_core_check.py "${drivers[@]}";
		else missing "no Core checker test executable is built"; fi
		step "Generated well-typed Core programs pass both checkers and print identically in C and Lean"
		if ((${#drivers[@]})); then PYTHONDONTWRITEBYTECODE=1 python3 ci/check_core_generated.py "${drivers[@]}";
		else missing "no Core checker test executable is built"; fi
		step "Heap Float spellings match Lean and BigInt digits match Python"
		heap_drivers=()
		for driver in build/{gcc,clang}/{debug,sanitize}/tests/test_heap.test; do
			if [[ -x "$driver" ]]; then heap_drivers+=("$driver"); fi
		done
		if ((${#heap_drivers[@]})); then python3 ci/check_heap_numbers.py "${heap_drivers[@]}";
		else missing "no heap test executable is built"; fi
		step "Compiled Core outcomes match the Lean VM model"
		vm_drivers=()
		for driver in build/{gcc,clang}/{debug,sanitize}/tests/test_vm.test; do
			if [[ -x "$driver" ]]; then vm_drivers+=("$driver"); fi
		done
		if ((${#vm_drivers[@]})); then PYTHONDONTWRITEBYTECODE=1 python3 ci/check_vm.py "${vm_drivers[@]}";
		else missing "no VM test executable is built"; fi
		step "Generated Core programs give the same outcome on the VM and in Lean"
		if ((${#vm_drivers[@]})); then PYTHONDONTWRITEBYTECODE=1 python3 ci/check_core_differential.py "${vm_drivers[@]}";
		else missing "no VM test executable is built"; fi
		step "Lowered checked-program fixtures match expected outcomes and Lean"
		lower_drivers=()
		for driver in build/{gcc,clang}/{debug,sanitize}/tests/test_lower.test; do
			if [[ -x "$driver" ]]; then lower_drivers+=("$driver"); fi
		done
		for driver in "${lower_drivers[@]}"; do
			compiler=${driver#build/}; compiler=${compiler%%/*}
			make --no-print-directory CC="$compiler" "${driver%/tests/*}/lower-source"
		done
		if ((${#lower_drivers[@]})); then PYTHONDONTWRITEBYTECODE=1 python3 ci/check_lower.py "${lower_drivers[@]}";
		else missing "no lowering test executable is built"; fi
		step "Source/check/lower programs match expected outcomes, Lean and the host-entry VM"
		for driver in "${lower_drivers[@]}"; do
			PYTHONDONTWRITEBYTECODE=1 python3 ci/check_lower_source.py "${driver%/tests/*}/lower-source" "${driver%/tests/*}/lower-source"
		done
		step "Primitive results and stops match Lean; large inputs keep their work bounds"
		prim_drivers=()
		for driver in build/{gcc,clang}/{debug,sanitize}/tests/test_prim.test; do
			if [[ -x "$driver" ]]; then prim_drivers+=("$driver"); fi
		done
		if ((${#prim_drivers[@]})); then python3 ci/check_primitives.py "${prim_drivers[@]}";
		else missing "no primitive test executable is built"; fi
		step "Fake capability operations match the Lean host model"
		host_drivers=()
		for driver in build/{gcc,clang}/{debug,sanitize}/tests/test_host.test; do
			if [[ -x "$driver" ]]; then host_drivers+=("$driver"); fi
		done
		if ((${#host_drivers[@]})); then PYTHONDONTWRITEBYTECODE=1 python3 ci/check_host.py "${host_drivers[@]}";
		else missing "no host test executable is built"; fi
	else
		missing "python3 is not installed"
	fi
else
	missing "elan (Lean's installer) is not installed"
fi
fi

if want drivers; then
step "Core code generation: deterministic writer-checked fixtures, corpora and generated programs"
if have python3; then
	drivers=()
	for driver in build/{gcc,clang}/{debug,sanitize}/tests/test_codegen.test; do
		if [[ -x "$driver" ]]; then drivers+=("$driver"); fi
	done
	if ((${#drivers[@]})); then python3 ci/check_codegen.py "${drivers[@]}";
	else missing "no code generation test executable is built"; fi
else
	missing "python3 is not installed"
fi
fi

if want static; then
step "Module dependencies: each module includes only what architecture section 2 allows"
make --no-print-directory deps

step "The library map: docs/design/library.md has a row for every primitive and host operation"
make --no-print-directory library-map

step "Docs-only changes: ci/docs_only.sh skips CI only for files no check reads"
sh ci/test_docs_only.sh

step "Pull request groups: ci/ci_groups.sh skips cbmc only when the change can't affect it"
sh ci/test_ci_groups.sh

step "The three tools' frozen tasks: hashes match and each reference passes its hidden tests"
if have python3; then python3 bench/tools/grade.py --check; else missing "python3 is not installed"; fi

step "The learnability protocol's Wilson and kappa calculator"
if have python3; then PYTHONDONTWRITEBYTECODE=1 python3 learnability/test_wilson.py; else missing "python3 is not installed"; fi

step "Formatting"
if have clang-format; then make --no-print-directory format-check; else missing "clang-format is not installed"; fi

step "Results of standard-library calls are used (clang-tidy)"
if have clang-tidy; then make --no-print-directory tidy; else missing "clang-tidy is not installed"; fi
fi

if want drivers; then
step "The source formatter: reference, examples, library and good grammar are fixed points"
formatter_driver=
for driver in build/{gcc,clang}/debug/tests/test_format.test; do
	if [[ -x "$driver" ]]; then formatter_driver=$driver; break; fi
done
if [[ -n "$formatter_driver" ]]; then "$formatter_driver" --fixed-points;
else missing "no source formatter test executable is built"; fi
fi

if want cbmc; then
step "CBMC: the bytecode reader is memory-safe on every small input (cbmc/README.md)"
if have cbmc && have goto-cc && have goto-instrument; then
	# The proofs and the check that every block they prove runs on some input take about eight
	# and six minutes one at a time, so they share the machine's processors.
	jobs=$(($(getconf _NPROCESSORS_ONLN) / 2))
	((jobs >= 1)) || jobs=1
	mkdir -p build
	MO_CBMC_JOBS=$jobs bash cbmc/prove.sh --reach >build/cbmc-reach.txt 2>&1 &
	reach=$!
	proofs_status=0
	MO_CBMC_JOBS=$jobs make --no-print-directory cbmc || proofs_status=$?
	step "CBMC: every block of each proved function runs on some input, so no proof is vacuous"
	reach_status=0
	wait "$reach" || reach_status=$?
	cat build/cbmc-reach.txt
	((proofs_status == 0 && reach_status == 0))
else
	missing "cbmc is not installed"
fi
fi

if ((${#skipped[@]} == 0)); then
	if [[ -n "$groups" ]]; then step "All checks in $groups passed"; else step "All checks passed"; fi
else
	noun="checks were"
	if ((${#skipped[@]} == 1)); then noun="check was"; fi
	step "${#skipped[@]} $noun skipped; the others passed"
	printf '  - %s\n' "${skipped[@]}"
	echo "CI treats a missing tool as an error. Install what is missing (.agents/setup does) before relying on this result."
fi
