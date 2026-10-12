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
export PATH="$HOME/.elan/bin:$PATH"
if have lake; then make --no-print-directory formal; else missing "elan (Lean's installer) is not installed"; fi

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
