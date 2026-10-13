#!/usr/bin/env bash
# Every check robertguss/mo's CI runs, in order. Run it before every push: the
# CI orb runs exactly this on each pull request it is sent (decisions D208, D214).
#
# Locally, a check whose tool is missing is skipped with a note, and the summary
# at the end counts what was skipped instead of claiming that all checks passed.
# In CI (CI=1) a missing tool is an error, so nothing is skipped silently.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

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

step "Build and test with gcc (warnings are errors)"
if have gcc; then make --no-print-directory test CC=gcc; else missing "gcc is not installed"; fi

step "Build and test with clang (warnings are errors)"
if have clang; then make --no-print-directory test CC=clang; else missing "clang is not installed"; fi

step "Tests under AddressSanitizer and UndefinedBehaviorSanitizer"
if have clang; then
	make --no-print-directory sanitize CC=clang
elif have gcc; then
	make --no-print-directory sanitize CC=gcc
else
	missing "no compiler with sanitizers is installed"
fi

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

step "Fuzz every target briefly"
if have clang && has_libfuzzer; then
	make --no-print-directory fuzz-smoke
else
	missing "clang with libFuzzer is not installed"
fi

step "The executable grammar: its tests, the examples and the reference's code parse"
if have bison && (have clang || have gcc); then
	if have clang; then make --no-print-directory grammar CC=clang; else make --no-print-directory grammar CC=gcc; fi
else
	missing "bison or a C compiler is not installed"
fi

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

step "Module dependencies: each module includes only what architecture section 2 allows"
make --no-print-directory deps

step "The library map: docs/design/library.md has a row for every primitive and host operation"
make --no-print-directory library-map

step "Formatting"
if have clang-format; then make --no-print-directory format-check; else missing "clang-format is not installed"; fi

step "Results of standard-library calls are used (clang-tidy)"
if have clang-tidy; then make --no-print-directory tidy; else missing "clang-tidy is not installed"; fi

if ((${#skipped[@]} == 0)); then
	step "All checks passed"
else
	noun="checks were"
	if ((${#skipped[@]} == 1)); then noun="check was"; fi
	step "${#skipped[@]} $noun skipped; the others passed"
	printf '  - %s\n' "${skipped[@]}"
	echo "CI treats a missing tool as an error. Install what is missing (.agents/setup does) before relying on this result."
fi
