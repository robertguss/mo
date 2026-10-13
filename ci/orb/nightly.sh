#!/usr/bin/env bash
# Runs robertguss/mo's checks on main's latest commit with long fuzzing, as the CI
# orb does once a night. Pull requests fuzz each target for a few seconds
# (ci/orb/run.sh); this gives every target minutes, on the code that merged.
#
#   bash ci/orb/nightly.sh
#
# Checks out origin/main in a fresh worktree, runs that commit's ci/check.sh with
# CI=1, a minimal environment and FUZZ_SECONDS (300 by default), keeps the full
# log and any crashing inputs, and exits with the check's status. It has its own
# clone, so pull requests' runs (run.sh) go on while it fuzzes.
set -uo pipefail

repo="robertguss/mo"
ci_home="${MO_CI_HOME:-$HOME/mo-ci}"
work="$ci_home/nightly"
logs="$ci_home/logs"
timeout_seconds="${MO_CI_NIGHTLY_TIMEOUT_SECONDS:-14400}"
fuzz_seconds="${MO_CI_FUZZ_SECONDS:-300}"

# Exit status: the check's own status when it ran (0 means every check passed),
# 64 for a usage error, 69 when the CI orb could not run the check at all.
if [[ $# -ne 0 || ! "$fuzz_seconds" =~ ^[1-9][0-9]*$ ]]; then
	echo "usage: [MO_CI_FUZZ_SECONDS=N] bash ci/orb/nightly.sh" >&2
	exit 64
fi
mkdir -p "$work" "$logs"

# One nightly run at a time. It doesn't take run.sh's lock: its clone is its own.
exec 9>"$ci_home/nightly.lock" || exit 69
flock 9 || exit 69

check_pid=""
check_home=""
tree=""
clone="$work/${repo//\//__}"

cleanup() {
	if [[ -n "$check_pid" ]]; then
		kill -s TERM -- "-$check_pid" 2>/dev/null
		wait "$check_pid" 2>/dev/null
		kill -s KILL -- "-$check_pid" 2>/dev/null
	fi
	if [[ -n "$check_home" ]]; then rm -rf "$check_home"; fi
	if [[ -n "$tree" ]]; then git -C "$clone" worktree remove --force "$tree" 2>/dev/null || rm -rf "$tree"; fi
}
trap cleanup EXIT
trap 'exit 143' TERM
trap 'exit 130' INT

if [[ ! -d "$clone/.git" ]] && ! git clone --quiet "https://github.com/$repo.git" "$clone"; then
	echo "error: could not clone $repo" >&2
	exit 69
fi
if ! git -C "$clone" fetch --quiet origin main; then
	echo "error: could not fetch $repo's main" >&2
	exit 69
fi
sha=$(git -C "$clone" rev-parse FETCH_HEAD) || exit 69
rm -rf "$work"/home.* "$work"/nightly-*
git -C "$clone" worktree prune
tree="$work/nightly-$sha"
if ! git -C "$clone" worktree add --quiet --detach "$tree" "$sha"; then
	tree=""
	echo "error: could not check out $sha" >&2
	exit 69
fi

check_home=$(mktemp -d "$work/home.XXXXXX") || exit 69
stamp=$(date -u +%Y-%m-%d)
logfile="$logs/${repo//\//__}-nightly-$stamp-$sha.log"
start=$(date +%s)
(
	cd "$tree" || exit 1
	exec 9>&-
	exec setsid env -i PATH="$PATH" HOME="$check_home" LANG=C.UTF-8 CI=1 FUZZ_SECONDS="$fuzz_seconds" \
		timeout --kill-after=30 "$timeout_seconds" bash ci/check.sh
) >"$logfile" 2>&1 &
check_pid=$!
status=0
wait "$check_pid" || status=$?
kill -s KILL -- "-$check_pid" 2>/dev/null
check_pid=""
took=$(($(date +%s) - start))

# Keep crashing inputs, which the worktree's removal would otherwise delete.
artifacts="$tree/build/fuzz/artifacts"
if [[ -d "$artifacts" ]] && [[ -n "$(ls -A "$artifacts" 2>/dev/null)" ]]; then
	kept="$logs/${repo//\//__}-nightly-$stamp-$sha-artifacts"
	mkdir -p "$kept" && cp -R "$artifacts/." "$kept/"
	echo "fuzz artifacts: $kept"
fi

case "$status" in
0) outcome="all checks passed" ;;
124) outcome="checks timed out after ${timeout_seconds}s" ;;
137) outcome="checks were killed after ${took}s (timeout or out of memory)" ;;
*) outcome="checks failed (exit status $status)" ;;
esac
echo "CI orb nightly: $outcome on main at ${sha:0:7} in ${took}s, fuzzing ${fuzz_seconds}s per target."
echo "log: $logfile"
if ((status != 0)); then
	echo "--- last 40 lines of the log ---"
	tail -n 40 "$logfile"
fi
exit "$status"
