#!/usr/bin/env bash
# The CI orb's job runner (decision D208). The mo-ci plugin queues one job per
# pushed commit; this loop runs them one at a time with the commit's own
# ci/check.sh and reports a "mo-ci" commit status to GitHub. On a failure it
# also comments the end of the log on the commit's open pull requests.
#
# The check runs in its own process group with a minimal environment, so none
# of the orb's credentials reach it. Stopping the runner stops the check and
# everything it started; a job that a restart interrupted goes back to the
# queue and runs again.
#
# Runs only in the CI orb, as the orb service "mo-ci-runner"; see
# ci/orb/README.md.
set -uo pipefail

ci_home="${MO_CI_HOME:-$HOME/mo-ci}"
queue="$ci_home/queue"
running="$ci_home/running"
finished="$ci_home/done"
work="$ci_home/work"
logs="$ci_home/logs"
context="mo-ci"
timeout_seconds="${MO_CI_TIMEOUT_SECONDS:-1800}"
mkdir -p "$queue" "$running" "$finished" "$work" "$logs"

check_pid=""  # leads the process group of the check that is running, if any
check_home="" # that check's HOME
idle_pid=""   # the sleep between looks at the queue, if one is running

thread_url=""
if [[ -n "${AMP_URL:-}" && -n "${AMP_THREAD_ID:-}" ]]; then
	thread_url="${AMP_URL%/}/threads/$AMP_THREAD_ID"
fi

log() { printf '%s %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$*"; }

# Runs when the runner exits for any reason it can catch: stops the running check
# and everything the check started, and removes its HOME. The job stays in
# running/, and the next start of the runner puts it back in the queue.
cleanup() {
	if [[ -n "$idle_pid" ]]; then kill "$idle_pid" 2>/dev/null; fi
	if [[ -n "$check_pid" ]]; then
		# The check leads its own process group, so one signal reaches all of it. If
		# TERM is ignored, the check's own timeout --kill-after sends KILL.
		kill -s TERM -- "-$check_pid" 2>/dev/null
		wait "$check_pid" 2>/dev/null
		kill -s KILL -- "-$check_pid" 2>/dev/null
		check_pid=""
	fi
	if [[ -n "$check_home" ]]; then
		rm -rf "$check_home"
		check_home=""
	fi
}
trap cleanup EXIT
trap 'exit 143' TERM # The EXIT trap stops the check.
trap 'exit 130' INT

# Sleeps in the background and waits, because bash runs a trap for a signal that
# arrives during a foreground command only after the command ends.
idle() {
	sleep 3 &
	idle_pid=$!
	wait "$idle_pid"
	idle_pid=""
}

# Calls the GitHub API. Amp's own GitHub credential in orbs can push and comment but
# may not write commit statuses, so the runner uses MO_CI_GITHUB_TOKEN when it is set:
# a fine-grained token for this repository with "Commit statuses" and "Pull requests"
# write access (see ci/orb/README.md). The check itself never sees it (env -i).
github() {
	if [[ -n "${MO_CI_GITHUB_TOKEN:-}" ]]; then
		GH_TOKEN="$MO_CI_GITHUB_TOKEN" gh api "$@"
	else
		gh api "$@"
	fi
}

post_status() { # repo sha state description
	local args=(-f "state=$3" -f "context=$context" -f "description=$4")
	if [[ -n "$thread_url" ]]; then args+=(-f "target_url=$thread_url"); fi
	local attempt
	for attempt in 1 2 3; do
		if github --silent "repos/$1/statuses/$2" "${args[@]}"; then return 0; fi
		sleep $((attempt * 2))
	done
	log "could not post the $3 status for $1@$2 after 3 attempts; if GitHub said 403, MO_CI_GITHUB_TOKEN is missing or lacks Commit statuses write access"
	return 1
}

comment_failure() { # repo sha logfile reason
	local prs pr body
	prs=$(github "repos/$1/commits/$2/pulls" --jq '.[] | select(.state == "open") | .number' 2>/dev/null) || return 0
	[[ -z "$prs" ]] && return 0
	body=$(
		printf 'CI orb: %s on %s. The last 60 lines of the log:\n\n' "$4" "${2:0:7}"
		printf '~~~\n'
		tail -n 60 "$3"
		printf '~~~\n'
	)
	for pr in $prs; do
		github --silent "repos/$1/issues/$pr/comments" -f body="$body" || log "could not comment on $1#$pr"
	done
}

run_job() { # job file
	local repo sha
	repo=$(jq -r '.repo' "$1" 2>/dev/null)
	sha=$(jq -r '.sha' "$1" 2>/dev/null)
	if [[ ! "$repo" =~ ^robertguss/[A-Za-z0-9._-]+$ || ! "$sha" =~ ^[0-9a-f]{40}$ ]]; then
		log "skipping a malformed job: $1"
		return
	fi
	local name="${repo//\//__}"
	local clone="$work/$name"
	local tree="$work/$name-$sha"
	local logfile="$logs/$name-$sha.log"

	log "running $repo@$sha"
	post_status "$repo" "$sha" pending "Running in the CI orb"
	if [[ ! -d "$clone/.git" ]] && ! git clone --quiet "https://github.com/$repo.git" "$clone"; then
		post_status "$repo" "$sha" error "The CI orb could not clone the repository"
		return
	fi
	git -C "$clone" fetch --quiet origin "$sha" 2>/dev/null || git -C "$clone" fetch --quiet origin
	rm -rf "$tree"
	git -C "$clone" worktree prune
	if ! git -C "$clone" worktree add --quiet --detach "$tree" "$sha"; then
		post_status "$repo" "$sha" error "The CI orb could not check out the commit"
		return
	fi

	if ! check_home=$(mktemp -d "$work/home.XXXXXX"); then
		check_home=""
		post_status "$repo" "$sha" error "The CI orb could not create a home directory for the checks"
		return
	fi
	local start status=0 took
	start=$(date +%s)
	# The code under test gets only what it needs: no API keys, and a fresh HOME.
	# setsid makes the check lead a new process group, whose id is $check_pid.
	(
		cd "$tree" || exit 1
		exec setsid env -i PATH="$PATH" HOME="$check_home" LANG=C.UTF-8 CI=1 \
			timeout --kill-after=30 "$timeout_seconds" bash ci/check.sh
	) >"$logfile" 2>&1 &
	check_pid=$!
	wait "$check_pid" || status=$?
	check_pid=""
	rm -rf "$check_home"
	check_home=""
	took=$(($(date +%s) - start))

	if ((status == 0)); then
		post_status "$repo" "$sha" success "All checks passed in ${took}s"
	elif ((status == 124)); then
		post_status "$repo" "$sha" failure "Timed out after ${timeout_seconds}s"
		comment_failure "$repo" "$sha" "$logfile" "checks timed out after ${timeout_seconds}s"
	elif ((status == 137)); then
		# SIGKILL: timeout's last resort after a timeout, or the kernel's out-of-memory killer.
		post_status "$repo" "$sha" failure "Killed after ${took}s (timeout or out of memory)"
		comment_failure "$repo" "$sha" "$logfile" "checks were killed after ${took}s (timeout or out of memory)"
	else
		post_status "$repo" "$sha" failure "Checks failed after ${took}s"
		comment_failure "$repo" "$sha" "$logfile" "checks failed"
	fi
	git -C "$clone" worktree remove --force "$tree" 2>/dev/null || rm -rf "$tree"
	log "finished $repo@$sha with exit status $status in ${took}s"
}

log "mo-ci-runner started"

# A job still in running/ was interrupted by a restart: it goes back in the queue
# and runs again, instead of staying pending forever. Homes left by a runner that
# was killed are removed too.
for interrupted in "$running"/*.json; do
	[[ -e "$interrupted" ]] || continue # the pattern matched nothing
	if mv "$interrupted" "$queue/"; then log "requeued the interrupted job ${interrupted##*/}"; fi
done
rm -rf "$work"/home.*

while true; do
	next=$(ls -1tr "$queue"/*.json 2>/dev/null | head -n 1)
	if [[ -z "$next" ]]; then
		idle
		continue
	fi
	claimed="$running/$(basename "$next")"
	mv "$next" "$claimed" 2>/dev/null || continue
	run_job "$claimed"
	mv "$claimed" "$finished/"
done
