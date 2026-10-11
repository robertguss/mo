#!/usr/bin/env bash
# The CI orb's job runner (decision D208). The mo-ci plugin queues one job per
# pushed commit; this loop runs them one at a time with the commit's own
# ci/check.sh and reports a "mo-ci" commit status to GitHub. On a failure it
# also comments the end of the log on the commit's open pull requests.
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

thread_url=""
if [[ -n "${AMP_URL:-}" && -n "${AMP_THREAD_ID:-}" ]]; then
	thread_url="${AMP_URL%/}/threads/$AMP_THREAD_ID"
fi

log() { printf '%s %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$*"; }

post_status() { # repo sha state description
	local args=(-f "state=$3" -f "context=$context" -f "description=$4")
	if [[ -n "$thread_url" ]]; then args+=(-f "target_url=$thread_url"); fi
	gh api --silent "repos/$1/statuses/$2" "${args[@]}" || log "could not post the $3 status for $1@$2"
}

comment_failure() { # repo sha logfile reason
	local prs pr body
	prs=$(gh api "repos/$1/commits/$2/pulls" --jq '.[] | select(.state == "open") | .number' 2>/dev/null) || return 0
	[[ -z "$prs" ]] && return 0
	body=$(
		printf 'CI orb: %s on %s. The last 60 lines of the log:\n\n' "$4" "${2:0:7}"
		printf '~~~\n'
		tail -n 60 "$3"
		printf '~~~\n'
	)
	for pr in $prs; do
		gh api --silent "repos/$1/issues/$pr/comments" -f body="$body" || log "could not comment on $1#$pr"
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

	local start status=0 took
	start=$(date +%s)
	(cd "$tree" && CI=1 timeout --kill-after=30 "$timeout_seconds" bash ci/check.sh) >"$logfile" 2>&1 || status=$?
	took=$(($(date +%s) - start))

	if ((status == 0)); then
		post_status "$repo" "$sha" success "All checks passed in ${took}s"
	elif ((status == 124 || status == 137)); then
		post_status "$repo" "$sha" failure "Timed out after ${timeout_seconds}s"
		comment_failure "$repo" "$sha" "$logfile" "checks timed out after ${timeout_seconds}s"
	else
		post_status "$repo" "$sha" failure "Checks failed after ${took}s"
		comment_failure "$repo" "$sha" "$logfile" "checks failed"
	fi
	git -C "$clone" worktree remove --force "$tree" 2>/dev/null || rm -rf "$tree"
	log "finished $repo@$sha with exit status $status in ${took}s"
}

log "mo-ci-runner started"
while true; do
	next=$(ls -1tr "$queue"/*.json 2>/dev/null | head -n 1)
	if [[ -z "$next" ]]; then
		sleep 3
		continue
	fi
	claimed="$running/$(basename "$next")"
	mv "$next" "$claimed" 2>/dev/null || continue
	run_job "$claimed"
	mv "$claimed" "$finished/"
done
