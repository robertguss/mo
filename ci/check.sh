#!/usr/bin/env bash
# Every check robertguss/mo has, in order, except that the drivers, bench and lean groups run
# their checks side by side. GitHub's nightly workflow runs rust, drivers, bench, lean and static
# on main in parallel jobs. Before a pull request, run `bash ci/check.sh rust`.
#
# All compiler and runtime checks drive the pure-Rust `mo` binary (`target/release/mo`, or $MO_BIN).
# MO_SIDE_BY_SIDE caps how many checks run side by side.
#
# A check whose tool is missing is skipped with a note, and the summary at the end counts what
# was skipped instead of claiming that all checks passed. Set CI=1 to make a missing tool an
# error instead, so nothing is skipped silently.
#
# Output starts with `check.sh started: pid N`. A run with valid arguments ends in the line
# `check.sh finished: pass` or `check.sh finished: fail`, and the fail marker comes after
# `failed group: NAME`. ci/wait-for-results reads these lines; AGENTS.md shows how to use it.
#
# With no arguments every check runs. Arguments pick groups of checks; together the groups are
# every check:
#   rust        pure-Rust workspace: fmt, clippy, unit/doc tests, rustdoc, safety/linkage audit, release build
#   drivers     black-box drivers of the built pure-Rust mo executable
#   bench       benchmark v0, the three Mo-written tools and the learnability protocol's calculators
#   lean        Mo Core in Lean, and the pure-Rust <-> Lean differential checks
#   static      executable Bison grammar, library map, editor grammars and reference conformance
#   models      finite TLA+ export, capture and swap models (TLC)
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
echo "check.sh started: pid $$"

all_groups=" rust drivers bench lean static models "
groups="$*"
for group in $groups; do
	if [[ "$all_groups" != *" $group "* ]]; then
		echo "usage: bash ci/check.sh [group...], where a group is one of:$all_groups" >&2
		exit 64
	fi
done
want() { [[ -z "$groups" || " $groups " == *" $1 "* ]]; }
entered_group=
enter() { want "$1" && entered_group=$1; }

skipped=()

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

# Checks that don't depend on each other run side by side, as many at a time as there are
# processors, or MO_SIDE_BY_SIDE. `later TITLE COMMAND...` starts one in the background, and
# `report` prints each one's output under its title as it finishes, with how long it took, and
# fails if any of them failed.
later_limit=${MO_SIDE_BY_SIDE:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}
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
stop_later() {
	local pid
	for pid in "${later_pids[@]}"; do
		pkill -P "$pid" 2>/dev/null || true
		kill "$pid" 2>/dev/null || true
	done
	if [[ -n "$later_dir" ]]; then rm -rf "$later_dir"; fi
}
# The marker can't use $?: a signal leaves it 0 here, so only the script's last line means pass.
passed=
finish() {
	stop_later
	if [[ -n "$passed" ]]; then
		echo "check.sh finished: pass"
	else
		echo "failed group: $entered_group"
		echo "check.sh finished: fail"
	fi
}
trap finish EXIT

mo_bin=${MO_BIN:-target/release/mo}
built_rust=0
ensure_rust_binaries() {
	if ((built_rust)); then return 0; fi
	if [[ -n "${MO_BIN:-}" && -x "$mo_bin" ]]; then
		built_rust=1
		return 0
	fi
	if ! have cargo; then
		missing "cargo is not installed"
		return 1
	fi
	step "Build pure-Rust release binaries (mo, mo-fe, mo-test-cli, mo-test-deadline)"
	cargo build --release --bin mo --bin mo-fe --bin mo-test-cli --bin mo-test-deadline
	built_rust=1
	return 0
}

if enter rust; then
step "Pure-Rust workspace: formatting, clippy, tests, docs, safety & linkage audit"
if ! have cargo; then
	missing "cargo is not installed"
elif ! have python3; then
	missing "python3 is not installed"
else
	step "Rust formatting (cargo fmt --all -- --check)"
	cargo fmt --all -- --check
	step "Rust lints (cargo clippy --workspace --all-targets --all-features -- -D warnings)"
	cargo clippy --workspace --all-targets --all-features -- -D warnings
	step "Rust workspace tests (cargo test --workspace --all-targets)"
	cargo test --workspace --all-targets
	step "Rust doc tests (cargo test --workspace --doc)"
	cargo test --workspace --doc
	step "Rust documentation (RUSTDOCFLAGS=\"-D warnings\" cargo doc --workspace --no-deps)"
	RUSTDOCFLAGS="-D warnings" cargo doc --workspace --no-deps
	ensure_rust_binaries
	step "Rust crate safety/lint audit (#![forbid(unsafe_code)]) and dynamic linkage audit (ldd)"
	python3 ci/check_rust_frontend.py --audit-only --mo "$mo_bin"
fi
fi

if enter drivers; then
shared=${MO_SHARED_CHECKS:-1}
if have python3; then
	if ensure_rust_binaries && [[ -x "$mo_bin" ]]; then
		if ((shared)); then
			later "Check runner: timed checks run alone and failures propagate" python3 ci/test_check.py
			later "Waiting on check results: running, passed, failed, killed and missing runs" \
				python3 ci/test_wait_for_results.py
			later "Reference conformance: every rule has programs or an explicit gap" \
				python3 ci/check_conformance.py
			later "Reference conformance: coverage-check regression tests" \
				python3 ci/test_conformance.py
			later "Unicode 16: the pinned tables match the UCD files" \
				python3 ci/generate_unicode16.py --check
		fi
		later "The mo command: text/JSON goldens, argument matrix and JSON validity ($mo_bin)" \
			python3 ci/check_cli.py "$mo_bin"
		later "Authenticated operator views ($mo_bin)" python3 ci/check_operator.py "$mo_bin"
		later "Typed upgrades and checked restart ($mo_bin)" python3 ci/check_upgrades.py "$mo_bin"
		later "Recorded schema fallback ($mo_bin)" python3 ci/check_upgrade_fallback.py "$mo_bin"
		later "Upgrade adoption replay ($mo_bin)" python3 ci/check_upgrade_adoption.py "$mo_bin"
		later "Recorded-context upgrade checks ($mo_bin)" python3 ci/check_upgrade_context.py "$mo_bin"
		later "Live upgrades: push, streams, kills and recovery ($mo_bin)" \
			python3 ci/check_upgrade_live.py "$mo_bin"
		later "Remote upgrades: proxy, tunnel and rate retry ($mo_bin)" \
			python3 ci/check_upgrade_remote.py "$mo_bin"
		later "Private state fetch ($mo_bin)" python3 ci/check_fetch.py "$mo_bin"
		later "The Http client against a live peer on loopback ($mo_bin)" \
			python3 ci/check_network.py "$mo_bin"
		later "Offline history, replay and query ($mo_bin)" python3 ci/check_offline.py "$mo_bin"
		later "Immutable state exports ($mo_bin)" python3 ci/check_export.py "$mo_bin"
		later "Service simulation: deterministic lifecycle and real persistence ($mo_bin)" \
			python3 ci/check_simulation.py "$mo_bin"
		later "Task queue: simulation tests and the real service over loopback ($mo_bin)" \
			python3 ci/check_task_queue.py "$mo_bin"
		later "Task queue: SIGKILL a real mo serve and reconcile what its client saw ($mo_bin)" \
			python3 ci/check_kill_loop.py "$mo_bin" --kills "${MO_KILL_COUNT:-15}" \
			--seed "${MO_KILL_SEED:-1}" --keep
		later "Reference examples: check every block and run complete programs ($mo_bin)" \
			python3 ci/check_reference_examples.py "$mo_bin"
		later "Reference examples: driver regression tests ($mo_bin)" \
			python3 ci/test_reference_examples.py "$mo_bin"
		later "The golden corpus: whole programs match their expected output and exit status ($mo_bin)" \
			python3 ci/check_golden.py "$mo_bin"
		later "The library: every file passes mo test ($mo_bin)" \
			python3 ci/check_lib.py "$mo_bin"
		later "Crypto & Bytes encodings: differential cross-check vs Python stdlib ($mo_bin)" \
			python3 ci/check_crypto.py "$mo_bin"
		later "Tools and services: every program in tools/ and services/mo-*/ passes mo test ($mo_bin)" \
			python3 ci/check_programs.py "$mo_bin"
		later "Every tracked Mo source: formatter idempotence, trees, check and execution ($mo_bin)" \
			python3 ci/check_format.py "$mo_bin"
		later "Every error code has a message test or reachability explanation ($mo_bin)" \
			python3 ci/check_errors.py "$mo_bin"

		report_status=0
		report || report_status=$?
		# Timed alone, after the checks that ran side by side, so their load does not inflate it.
		# It runs even when one of them failed.
		timing_status=0
		if ((shared)); then
			step "mo check stays under 0.1 s on each sample ($mo_bin)"
			python3 ci/check_check_time.py "$mo_bin" || timing_status=$?
		fi
		if ((report_status != 0 || timing_status != 0)); then exit 1; fi
	else
		missing "no mo executable is built"
	fi
else
	missing "python3 is not installed"
fi
fi

if enter bench; then
step "Benchmark v0, the three Mo-written tools and the learnability protocol"
if have python3; then
	if ensure_rust_binaries && [[ -x "$mo_bin" ]]; then
		later "The session-recording harness: a recorded session's log reads to tally's example table" \
			env PYTHONDONTWRITEBYTECODE=1 python3 learnability/harness/test_session.py "$mo_bin"
		later "Benchmark v0: tasks frozen, references pass, broken solutions and start folders fail ($mo_bin)" \
			python3 bench/v0/grade.py --check --mo "$mo_bin"
		later "The learnability protocol's Wilson and kappa calculator" \
			env PYTHONDONTWRITEBYTECODE=1 python3 learnability/test_wilson.py
		later "The round report's first-retry numbers, from the reference tally" \
			env PYTHONDONTWRITEBYTECODE=1 python3 learnability/test_retry.py
		later "The Phase 4 evaluation's between-round comparison and its power" \
			env PYTHONDONTWRITEBYTECODE=1 python3 learnability/phase4/test_compare.py
		later "Phase 4 scenario fixtures: a step list builds, renders, checks and keys" \
			env PYTHONDONTWRITEBYTECODE=1 python3 learnability/phase4/test_fixture.py "$mo_bin"
		later "Phase 4 grading: hidden tests, answers and each variant's controls" \
			env PYTHONDONTWRITEBYTECODE=1 python3 learnability/phase4/test_grade.py "$mo_bin"
		later "Phase 4 fix check against the grader: a fix the grader passes is never not_fixed" \
			env PYTHONDONTWRITEBYTECODE=1 python3 learnability/phase4/test_offline.py "$mo_bin"
		later "Phase 4 recording proxy: bodies, statuses, no token, and the record match" \
			env PYTHONDONTWRITEBYTECODE=1 python3 learnability/phase4/test_proxy.py "$mo_bin"
		later "Phase 4 publication scanner: a token in any transport encoding is refused" \
			env PYTHONDONTWRITEBYTECODE=1 python3 learnability/phase4/test_scan.py
		later "Phase 4 published grades: the hidden tests' names and details are left out" \
			env PYTHONDONTWRITEBYTECODE=1 python3 learnability/phase4/test_publish.py
		report_status=0
		report || report_status=$?
		# The session harness's tests time a killed session, so they too run alone. They run
		# stub sessions as a user of their own, which needs passwordless sudo.
		harness_status=0
		if sudo -n true 2>/dev/null; then
			step "Phase 4 session harness: stub sessions as their own user, killed, recorded and graded ($mo_bin)"
			PYTHONDONTWRITEBYTECODE=1 python3 learnability/phase4/test_harness.py "$mo_bin" || harness_status=$?
		else
			missing "passwordless sudo is not available, so the Phase 4 session harness is not tested"
		fi
		# The frozen tasks' hidden tests have time limits, so they run alone, after the checks
		# that ran side by side, and even when one of those failed.
		step "The three tools' frozen tasks: hashes match and each reference passes its hidden tests"
		frozen_status=0
		python3 bench/tools/grade.py --check || frozen_status=$?
		step "The three tools: tests, frozen tasks and repository checks ($mo_bin)"
		tools_status=0
		PYTHONDONTWRITEBYTECODE=1 python3 ci/check_tools.py "$mo_bin" || tools_status=$?
		if ((report_status != 0 || harness_status != 0 || frozen_status != 0 || tools_status != 0)); then exit 1; fi
	else
		missing "no mo executable is built"
	fi
else
	missing "python3 is not installed"
fi
fi

if enter lean; then
step "Mo Core in Lean: the model builds and every proof checks"
for elan_home in "${ELAN_HOME:-}" /opt/elan "$HOME/.elan"; do
	if [[ -n "$elan_home" && -x "$elan_home/bin/lake" ]]; then
		export ELAN_HOME="$elan_home" PATH="$elan_home/bin:$PATH"
		break
	fi
done
if have lake; then
	(cd formal && lake build MoCore mocore)
	sh ci/test_mocore.sh
	step "Core text fixtures match the Lean reference printer"
	sh ci/check_core_fixtures.sh
	step "Offline-diagnosis theorems pin only the standard axioms; a planted sorry is caught"
	sh ci/check_lean_axioms.sh
	if have python3; then
		if ensure_rust_binaries && [[ -x "$mo_bin" ]]; then
			later "Defect replays: mo replay and Lean stop the same way on hand-written handler inputs" \
				env PYTHONDONTWRITEBYTECODE=1 python3 ci/check_offline_core.py "$mo_bin"
			later "Core differential: generated Core programs give the same outcome in Lean and the VM (target/release/mo-fe)" \
				env PYTHONDONTWRITEBYTECODE=1 MO_CORE_DIFF="${MO_CORE_DIFF:-200}" \
				python3 ci/check_core_differential.py target/release/mo-fe
			later "Pure-Rust <-> Lean MoCore differential parity (fixtures, Core checker, generated Core, lowering)" \
				env PYTHONDONTWRITEBYTECODE=1 python3 ci/check_rust_frontend.py \
				--mo "$mo_bin" --mo-fe target/release/mo-fe \
				--skip-build --require-lean --lean-only
			report
		else
			missing "no mo executable is built"
		fi
	else
		missing "python3 is not installed"
	fi
else
	missing "elan (Lean's installer) is not installed"
fi
fi

if enter static; then
step "The executable grammar: its tests, the examples, the reference and benchmark v0 parse"
if have bison && (have clang || have gcc || have cc); then
	if have clang; then
		make --no-print-directory -C grammar grammar CC=clang
	elif have gcc; then
		make --no-print-directory -C grammar grammar CC=gcc
	else
		make --no-print-directory -C grammar grammar CC=cc
	fi
else
	missing "bison or a C compiler is not installed"
fi

step "The library map: docs/design/library.md has a row for every primitive and host operation"
if ensure_rust_binaries; then
	sh ci/library_map.sh
	sh ci/test_library_map.sh
fi

step "Editor grammars, queries and installer (Zed & Cursor/VS Code)"
if have python3; then python3 ci/check_editors.py; else missing "python3 is not installed"; fi

step "Reference conformance: every rule has programs or an explicit gap"
if have python3; then
	python3 ci/check_conformance.py
	python3 ci/test_conformance.py
else
	missing "python3 is not installed"
fi
fi

if enter models; then
step "Finite export, capture and swap models and negative controls"
model_status=0
python3 ci/check_models.py || model_status=$?
if ((model_status == 77)); then
	missing "Java or pinned TLC jar is not installed (see models/README.md)"
elif ((model_status != 0)); then
	exit "$model_status"
fi
fi

if ((${#skipped[@]} == 0)); then
	if [[ -n "$groups" ]]; then step "All checks in $groups passed"; else step "All checks passed"; fi
else
	noun="checks were"
	if ((${#skipped[@]} == 1)); then noun="check was"; fi
	step "${#skipped[@]} $noun skipped; the others passed"
	printf '  - %s\n' "${skipped[@]}"
	echo "CI=1 treats a missing tool as an error. Install what is missing before relying on this result."
fi
passed=1
