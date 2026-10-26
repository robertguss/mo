#!/usr/bin/env bash
# Runs robertguss/mo's checks on one pull request, as the CI orb does on request
# (decisions D208 and D214). An implementation thread sends the CI orb a pull
# request; the CI orb runs this script and replies with the result.
#
#   bash ci/orb/run.sh <pull request number>
#
# Checks out the pull request's head commit in a fresh worktree, runs that
# commit's ci/check.sh with CI=1 and a minimal environment, keeps the full log,
# comments the result on the pull request, and exits with the check's status.
set -uo pipefail

repo="robertguss/mo"
ci_home="${MO_CI_HOME:-$HOME/mo-ci}"
work="$ci_home/work"
logs="$ci_home/logs"
# Four hours: a full ci/check.sh includes CBMC proofs plus reach (~57 minutes
# back to back) on top of gcc, clang, sanitize, fuzz, drivers, lean and static.
timeout_seconds="${MO_CI_TIMEOUT_SECONDS:-14400}"

# Exit status: the check's own status when it ran (0 means every check passed),
# 64 for a usage error, 69 when the CI orb could not run the check at all.
pr="${1:-}"
if [[ $# -ne 1 || ! "$pr" =~ ^[1-9][0-9]*$ ]]; then
	echo "usage: bash ci/orb/run.sh <pull request number>" >&2
	exit 64
fi
mkdir -p "$work" "$logs"

# One run at a time: runs share a clone, so a second run waits for the first.
exec 9>"$ci_home/lock" || exit 69
flock 9 || exit 69

check_pid="" # leads the process group of the running check, if any
check_home=""
tree=""
clone="$work/${repo//\//__}"

# Stops the check and everything it started if this script is interrupted, and
# removes the check's HOME and worktree.
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

if ! info=$(gh pr view "$pr" --repo "$repo" --json state,isCrossRepository,headRefOid \
	--jq '[.state, (.isCrossRepository | tostring), .headRefOid] | join(" ")'); then
	echo "error: could not read $repo#$pr" >&2
	exit 69
fi
read -r state cross sha <<<"$info"
if [[ "$state" != OPEN ]]; then
	echo "error: $repo#$pr is $state; the CI orb checks open pull requests" >&2
	exit 69
fi
if [[ "$cross" != false ]]; then
	echo "error: $repo#$pr comes from a fork; the CI orb runs only this repository's branches" >&2
	exit 69
fi
if [[ ! "$sha" =~ ^[0-9a-f]{40}$ ]]; then
	echo "error: unexpected head commit for $repo#$pr" >&2
	exit 69
fi

if [[ ! -d "$clone/.git" ]] && ! git clone --quiet "https://github.com/$repo.git" "$clone"; then
	echo "error: could not clone $repo" >&2
	exit 69
fi
if ! git -C "$clone" fetch --quiet origin "pull/$pr/head"; then
	echo "error: could not fetch $repo#$pr" >&2
	exit 69
fi
# Leftovers from a run that was killed (with KILL, nothing could clean up). Safe
# to remove: holding the lock means no other run is active.
rm -rf "$work"/home.* "$work"/pr[0-9]*-[0-9a-f]*
git -C "$clone" worktree prune
tree="$work/pr$pr-$sha"
if ! git -C "$clone" worktree add --quiet --detach "$tree" "$sha"; then
	tree=""
	echo "error: could not check out $sha" >&2
	exit 69
fi

check_home=$(mktemp -d "$work/home.XXXXXX") || exit 69
logfile="$logs/${repo//\//__}-pr$pr-$sha.log"
start=$(date +%s)
# The code under test gets only what it needs: no API keys, and a fresh HOME.
# setsid makes the check lead a new process group, so stopping it stops everything.
(
	cd "$tree" || exit 1
	exec 9>&- # the check must not hold the lock
	exec setsid env -i PATH="$PATH" HOME="$check_home" LANG=C.UTF-8 CI=1 \
		timeout --kill-after=30 "$timeout_seconds" bash ci/check.sh
) >"$logfile" 2>&1 &
check_pid=$!
status=0
wait "$check_pid" || status=$?
kill -s KILL -- "-$check_pid" 2>/dev/null # anything the check left running in the background
check_pid=""
took=$(($(date +%s) - start))

case "$status" in
0) outcome="all checks passed" ;;
124) outcome="checks timed out after ${timeout_seconds}s" ;;
137) outcome="checks were killed after ${took}s (timeout or out of memory)" ;;
*) outcome="checks failed (exit status $status)" ;;
esac
summary="CI orb: $outcome on ${sha:0:7} in ${took}s."

if ((status == 0)); then
	body="$summary"
else
	body=$(
		printf '%s The last 60 lines of the log:\n\n~~~\n' "$summary"
		tail -n 60 "$logfile" | cut -c1-500
		printf '~~~\n'
	)
fi
if ! gh api --silent "repos/$repo/issues/$pr/comments" -f body="$body"; then
	echo "warning: could not comment on $repo#$pr" >&2
fi

echo "$summary"
echo "log: $logfile"
if ((status != 0)); then
	echo "--- last 40 lines of the log ---"
	tail -n 40 "$logfile"
fi
exit "$status"
