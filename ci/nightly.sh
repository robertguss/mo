#!/bin/sh
# The bookkeeping for the nightly runs on main (.github/workflows/nightly.yml and differential.yml),
# through the gh command line. ci/orb/README.md says how the nightly tier works.
#
#   ci/nightly.sh decide WORKFLOW EVENT HEAD
#       Prints "run PREVIOUS" or "skip PREVIOUS", where PREVIOUS is the commit the last finished
#       run of WORKFLOW on main tested (empty when there is none). Only a scheduled run that finds
#       main still at PREVIOUS is skipped; a manual run always runs.
#   ci/nightly.sh report CHECK RESULT HEAD PREVIOUS RUN_URL
#       Keeps one open issue labelled `nightly` per failing check: files it on a first failure
#       (failure, cancelled, timed_out), comments while the check stays red, and comments and
#       closes it once the check succeeds. Any other result, such as skipped, does nothing.
#
# A gh failure never fails the night: it prints a warning annotation, shown on the run's summary
# page, and exits 0.
set -eu

warn() {
    echo "::warning title=nightly bookkeeping::$1" >&2
}

decide() {
    workflow=$1 event=$2 head=$3
    # A cancelled run tested nothing, so compare with the last run that finished its checks.
    if ! previous=$(gh run list --workflow "$workflow" --branch main --limit 20 \
        --json headSha,conclusion \
        --jq '[.[] | select(.conclusion == "success" or .conclusion == "failure")][0].headSha // ""'); then
        warn "could not list earlier runs of $workflow; running anyway"
        previous=
    fi
    if [ "$event" = schedule ] && [ -n "$previous" ] && [ "$previous" = "$head" ]; then
        echo "skip $previous"
    else
        echo "run $previous"
    fi
}

report() {
    check=$1 result=$2 head=$3 previous=$4 run_url=$5
    case "$result" in
    success | failure | cancelled | timed_out) ;;
    *) return 0 ;;
    esac

    title="Nightly: $check failing on main"
    if ! number=$(gh issue list --label nightly --state open --limit 100 --json number,title \
        --jq ".[] | select(.title == \"$title\") | .number"); then
        warn "could not list the nightly issues to report $check ($result)"
        return 0
    fi

    if [ -n "$previous" ]; then
        range="Commits since the previous nightly run: ${GITHUB_SERVER_URL:-https://github.com}/${GITHUB_REPOSITORY:-robertguss/mo}/compare/$previous...$head"
    else
        range="No earlier nightly run to compare with."
    fi

    if [ "$result" = success ]; then
        if [ -n "$number" ]; then
            gh issue comment "$number" --body "Passed again at $head: $run_url" &&
                gh issue close "$number" ||
                warn "could not close the nightly issue for $check"
        fi
        return 0
    fi

    if [ -n "$number" ]; then
        gh issue comment "$number" --body "Still failing ($result) at $head.
$range
Run: $run_url" || warn "could not comment on the nightly issue for $check"
        return 0
    fi

    gh label create nightly --force --color B60205 \
        --description "A check failing in the nightly run on main" >/dev/null &&
        gh issue create --title "$title" --label nightly --body "\`$check\` ended with $result in the nightly run on main.

Tested commit: $head
$range
Run: $run_url

Fix it in a pull request. This issue closes itself when the check passes again." ||
        warn "could not file the nightly issue for $check ($result)"
}

case "${1:-}" in
decide)
    shift
    [ "$#" -eq 3 ] || { echo "usage: ci/nightly.sh decide WORKFLOW EVENT HEAD" >&2; exit 2; }
    decide "$@"
    ;;
report)
    shift
    [ "$#" -eq 5 ] || { echo "usage: ci/nightly.sh report CHECK RESULT HEAD PREVIOUS RUN_URL" >&2; exit 2; }
    report "$@"
    ;;
*)
    echo "usage: ci/nightly.sh decide WORKFLOW EVENT HEAD | report CHECK RESULT HEAD PREVIOUS RUN_URL" >&2
    exit 2
    ;;
esac
