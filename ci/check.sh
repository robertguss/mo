#!/usr/bin/env bash
# Every check robertguss/mo's CI runs, in order, except that the drivers and lean groups run their
# checks side by side. Run it before every push: the CI orb runs exactly this on each pull request
# it is sent (decisions D208, D214).
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
#   cbmc        the CBMC proofs of the bytecode reader, the arena, the verifier and the primitives (cbmc/README.md)
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

# Checks that don't depend on each other run side by side, as many at a time as there are
# processors. `later TITLE COMMAND...` starts one in the background, and `report` prints each
# one's output under its title as it finishes, with how long it took, and fails if any of them
# failed. A command that is a shell function runs without `set -e`, so chain its steps with &&.
later_limit=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)
later_dir=
later_titles=()
later_pids=()
later() {
	local title=$1
	shift
	if [[ -z "$later_dir" ]]; then later_dir=$(mktemp -d); fi
	local n=${#later_titles[@]}
	later_titles+=("$title")
	while (($(jobs -rp | wc -l) >= later_limit)); do sleep 0.2; done
	(
		started=$SECONDS
		status=0
		"$@" </dev/null || status=$?
		echo "($((SECONDS - started)) s)"
		echo "$status" >"$later_dir/$n.status"
	) >"$later_dir/$n.log" 2>&1 &
	later_pids+=($!)
}
report() {
	local n left=${#later_titles[@]} shown=() failed=()
	while ((left > 0)); do
		for n in "${!later_titles[@]}"; do
			if [[ -n "${shown[n]:-}" ]]; then continue; fi
			# A check killed before it wrote its status has failed.
			if [[ ! -e "$later_dir/$n.status" ]] && kill -0 "${later_pids[n]}" 2>/dev/null; then continue; fi
			step "${later_titles[n]}"
			cat "$later_dir/$n.log"
			if [[ "$(cat "$later_dir/$n.status" 2>/dev/null)" != 0 ]]; then failed+=("${later_titles[n]}"); fi
			shown[n]=1
			left=$((left - 1))
		done
		if ((left > 0)); then sleep 0.2; fi
	done
	wait
	rm -rf "$later_dir"
	later_dir=
	later_titles=()
	later_pids=()
	if ((${#failed[@]})); then
		echo >&2
		printf 'failed: %s\n' "${failed[@]}" >&2
		return 1
	fi
}
# If the script stops early (a missing tool under CI=1, a failed build), checks it started
# don't outlive it.
stop_later() {
	local pid
	for pid in "${later_pids[@]}"; do
		pkill -P "$pid" 2>/dev/null || true
		kill "$pid" 2>/dev/null || true
	done
	if [[ -n "$later_dir" ]]; then rm -rf "$later_dir"; fi
}
trap stop_later EXIT

built() { # sets drivers to the built executables at $1 under build/<compiler>/<mode>/
	local driver
	drivers=()
	for driver in build/{gcc,clang}/{debug,sanitize}/"$1"; do
		if [[ -x "$driver" ]]; then drivers+=("$driver"); fi
	done
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
# Each script below checks the executables it is given one after another, so each gets one
# executable at a time and they all run side by side (`later`), the longest first.
if have python3; then
	built mo
	if ((${#drivers[@]})); then
		for driver in "${drivers[@]}"; do
			later "The mo command: text/JSON goldens, argument matrix and JSON validity ($driver)" \
				python3 ci/check_cli.py "$driver"
		done
		for driver in "${drivers[@]}"; do
			later "The golden corpus: whole programs match their expected output and exit status ($driver)" \
				python3 ci/check_golden.py "$driver"
		done
		for driver in "${drivers[@]}"; do
			later "Every error code has a test of its exact message ($driver)" \
				python3 ci/check_errors.py "$driver"
		done
		later "The session-recording harness: a recorded session's log reads to tally's example table" \
			env PYTHONDONTWRITEBYTECODE=1 python3 learnability/harness/test_session.py "${drivers[@]}"
	else
		missing "no mo command executable is built"
	fi

	built tests/test_bytecode_text.test
	if ((${#drivers[@]})); then
		later "Bytecode text: repeated layout and BigInt conversion stay within work ceilings" \
			python3 ci/check_bytecode_text.py "${drivers[@]}"
	else
		missing "no bytecode text test executable is built"
	fi

	built tests/test_codegen.test
	if ((${#drivers[@]})); then
		later "Core code generation: deterministic writer-checked fixtures, corpora and generated programs" \
			python3 ci/check_codegen.py "${drivers[@]}"
	else
		missing "no code generation test executable is built"
	fi

	later "Unicode 16: the pinned tables match the UCD files" python3 ci/generate_unicode16.py --check
	built tests/test_unicode16.test
	if ((${#drivers[@]})); then
		later "Unicode 16: UCD conformance and long-text work bounds" python3 ci/check_unicode16.py "${drivers[@]}"
	else
		missing "no Unicode primitive test executable is built"
	fi
else
	missing "python3 is not installed"
fi

formatter_driver=
for driver in build/{gcc,clang}/debug/tests/test_format.test; do
	if [[ -x "$driver" ]]; then formatter_driver=$driver; break; fi
done
if [[ -n "$formatter_driver" ]]; then
	later "The source formatter: reference, examples, library and good grammar are fixed points" \
		"$formatter_driver" --fixed-points
else
	missing "no source formatter test executable is built"
fi
report
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
	if have python3; then
		# The comparisons below don't depend on each other, so they run side by side
		# (`later`). check_primitives goes first: its work bounds have timeouts, and they
		# keep their margin best while the others are still starting.
		built tests/test_prim.test
		if ((${#drivers[@]})); then
			later "Primitive results and stops match Lean; large inputs keep their work bounds" \
				python3 ci/check_primitives.py "${drivers[@]}"
		else
			missing "no primitive test executable is built"
		fi
		built tests/test_vm.test
		vm_drivers=("${drivers[@]}")
		if ((${#vm_drivers[@]})); then
			later "Compiled Core outcomes match the Lean VM model" \
				env PYTHONDONTWRITEBYTECODE=1 python3 ci/check_vm.py "${vm_drivers[@]}"
		else
			missing "no VM test executable is built"
		fi
		built tests/test_lower.test
		lower_drivers=("${drivers[@]}")
		for driver in "${lower_drivers[@]}"; do
			compiler=${driver#build/}; compiler=${compiler%%/*}
			make --no-print-directory CC="$compiler" "${driver%/tests/*}/lower-source"
		done
		for driver in "${lower_drivers[@]}"; do
			later "Source/check/lower programs match expected outcomes, Lean and the host-entry VM (${driver%/tests/*})" \
				env PYTHONDONTWRITEBYTECODE=1 python3 ci/check_lower_source.py "${driver%/tests/*}/lower-source" "${driver%/tests/*}/lower-source"
		done
		built tests/test_heap.test
		if ((${#drivers[@]})); then
			later "Heap Float spellings match Lean and BigInt digits match Python" \
				python3 ci/check_heap_numbers.py "${drivers[@]}"
		else
			missing "no heap test executable is built"
		fi
		built tests/test_core_check.test
		if ((${#drivers[@]})); then
			later "Core checker verdicts match Lean on fixtures, generated variants and fuzz corpora" \
				python3 ci/check_core_check.py "${drivers[@]}"
			later "Generated well-typed Core programs pass both checkers and print identically in C and Lean" \
				env PYTHONDONTWRITEBYTECODE=1 python3 ci/check_core_generated.py "${drivers[@]}"
		else
			missing "no Core checker test executable is built"
		fi
		if ((${#vm_drivers[@]})); then
			later "Generated Core programs give the same outcome on the VM and in Lean" \
				env PYTHONDONTWRITEBYTECODE=1 python3 ci/check_core_differential.py "${vm_drivers[@]}"
		fi
		if ((${#lower_drivers[@]})); then
			later "Lowered checked-program fixtures match expected outcomes and Lean" \
				env PYTHONDONTWRITEBYTECODE=1 python3 ci/check_lower.py "${lower_drivers[@]}"
		else
			missing "no lowering test executable is built"
		fi
		built tests/test_host.test
		if ((${#drivers[@]})); then
			later "Fake capability operations match the Lean host model" \
				env PYTHONDONTWRITEBYTECODE=1 python3 ci/check_host.py "${drivers[@]}"
		else
			missing "no host test executable is built"
		fi
		report
	else
		missing "python3 is not installed"
	fi
else
	missing "elan (Lean's installer) is not installed"
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

step "The qgate report: keeps only new long functions and duplicates in src/ and lib/, never fails"
if have python3; then PYTHONDONTWRITEBYTECODE=1 python3 ci/test_qgate_report.py; else missing "python3 is not installed"; fi

step "The three tools' frozen tasks: hashes match and each reference passes its hidden tests"
if have python3; then python3 bench/tools/grade.py --check; else missing "python3 is not installed"; fi

step "The learnability protocol's Wilson and kappa calculator"
if have python3; then PYTHONDONTWRITEBYTECODE=1 python3 learnability/test_wilson.py; else missing "python3 is not installed"; fi

step "Formatting"
if have clang-format; then make --no-print-directory format-check; else missing "clang-format is not installed"; fi

step "Results of standard-library calls are used (clang-tidy)"
if have clang-tidy; then make --no-print-directory tidy; else missing "clang-tidy is not installed"; fi
fi

if want cbmc; then
step "CBMC: the bytecode reader on every small input, the arena, the verifier's transfer functions and the primitives' arithmetic and UTF-8 (cbmc/README.md)"
if have cbmc && have goto-cc && have goto-instrument; then
	# The proofs and the check that every block they prove runs on some input take about
	# thirty and twenty-seven minutes one at a time (the verifier's step proof alone about
	# eleven), so they share the machine's processors.
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
