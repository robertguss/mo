#!/bin/sh
# Tests ci/nightly.sh against a fake gh: the night is skipped only when a scheduled run finds main
# where the last tested run left it, and each check has at most one open `nightly` issue, filed
# when it fails, updated while it stays red, and closed when it passes.
set -eu

fail=0
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT

# The fake gh logs each call, one per line, and answers `run list` and `issue list` from canned
# JSON, filtered through jq as gh's --jq does. FAKE_GH_FAIL=1 makes every call fail.
mkdir "$dir/bin"
cat >"$dir/bin/gh" <<'EOF'
#!/bin/sh
printf '%s\n' "$*" >>"$FAKE_GH_LOG"
if [ "${FAKE_GH_FAIL:-}" = 1 ]; then exit 1; fi
filter=
json='[]'
case "$1 $2" in
"run list") json=${FAKE_GH_RUNS:-[]} ;;
"issue list") json=${FAKE_GH_ISSUES:-[]} ;;
esac
while [ "$#" -gt 0 ]; do
    if [ "$1" = --jq ]; then filter=$2; fi
    shift
done
if [ -n "$filter" ]; then printf '%s' "$json" | jq -r "$filter"; fi
EOF
chmod +x "$dir/bin/gh"

# run ARGS...: runs ci/nightly.sh with the fake gh answering from $runs_json and $issues_json
# (failing every call when $gh_fails is 1), then resets those three. Its standard output lands in
# $out, its standard error in $err, and the gh calls it made in $calls.
runs_json='[]' issues_json='[]' gh_fails=
run() {
    : >"$dir/log"
    out=$(PATH="$dir/bin:$PATH" FAKE_GH_LOG="$dir/log" FAKE_GH_RUNS="$runs_json" \
        FAKE_GH_ISSUES="$issues_json" FAKE_GH_FAIL="$gh_fails" GITHUB_SERVER_URL=https://github.com \
        GITHUB_REPOSITORY=robertguss/mo sh ci/nightly.sh "$@" 2>"$dir/err")
    err=$(cat "$dir/err")
    calls=$(cat "$dir/log")
    runs_json='[]' issues_json='[]' gh_fails=
}
expect_out() {
    if [ "$out" != "$1" ]; then
        echo "error: $2: ci/nightly.sh printed '$out', expected '$1'" >&2
        fail=1
    fi
}
expect_call() {
    if ! printf '%s\n' "$calls" | grep -q -- "$1"; then
        echo "error: $2: no gh call matched '$1'; calls were:" >&2
        printf '%s\n' "$calls" >&2
        fail=1
    fi
}
expect_no_call() {
    if printf '%s\n' "$calls" | grep -q -- "$1"; then
        echo "error: $2: a gh call matched '$1'; calls were:" >&2
        printf '%s\n' "$calls" >&2
        fail=1
    fi
}

runs='[{"headSha":"bbb","conclusion":"success","headBranch":"main"},{"headSha":"aaa","conclusion":"failure","headBranch":"main"}]'

# decide: skip only a scheduled run that finds main unchanged.
runs_json=$runs
run decide nightly.yml schedule bbb
expect_out "skip bbb" "scheduled run, main unchanged"
runs_json=$runs
run decide nightly.yml schedule ccc
expect_out "run bbb" "scheduled run, main moved"
runs_json=$runs
run decide nightly.yml workflow_dispatch bbb
expect_out "run bbb" "manual run, main unchanged"
runs_json='[]'
run decide nightly.yml schedule ccc
expect_out "run " "no earlier run"
# A cancelled run tested nothing, so the commit before it is the one to compare with.
runs_json='[{"headSha":"bbb","conclusion":"cancelled","headBranch":"main"},{"headSha":"aaa","conclusion":"success","headBranch":"main"}]'
run decide nightly.yml schedule bbb
expect_out "run aaa" "previous run cancelled"
expect_call "run list --workflow nightly.yml --branch main" "decide asks for runs on main only"
gh_fails=1
run decide nightly.yml schedule ccc
expect_out "run " "gh unreachable while deciding"

open='[{"number":7,"title":"Nightly: cbmc failing on main"}]'
other='[{"number":8,"title":"Nightly: lean failing on main"}]'

# report: a first failure files one issue naming the check, commit, range and run.
issues_json='[]'
run report cbmc failure ccc bbb https://run/1
expect_call "label create nightly" "first failure makes sure the label exists"
expect_call "issue create --title Nightly: cbmc failing on main --label nightly" "first failure files an issue"
expect_call "compare/bbb...ccc" "the issue names the commit range"
expect_call "https://run/1" "the issue links the run"
expect_no_call "issue comment" "first failure only files"

# A repeat failure comments on the open issue and files nothing.
issues_json=$open
run report cbmc failure ddd ccc https://run/2
expect_call "issue comment 7" "repeat failure comments"
expect_no_call "issue create" "repeat failure files nothing"

# A cancelled or timed-out check counts as a failure.
issues_json='[]'
run report cbmc cancelled ccc bbb https://run/3
expect_call "issue create" "cancelled check files an issue"
issues_json='[]'
run report cbmc timed_out ccc bbb https://run/3
expect_call "issue create" "timed-out check files an issue"

# A pass closes the open issue; another check's issue is left alone.
issues_json=$open
run report cbmc success eee ddd https://run/4
expect_call "issue comment 7" "pass comments on the open issue"
expect_call "issue close 7" "pass closes the open issue"
issues_json=$other
run report cbmc success eee ddd https://run/4
expect_no_call "issue close" "pass leaves another check's issue open"
expect_no_call "issue comment" "pass with no open issue says nothing"

# A skipped check (a quiet night) never closes an issue.
issues_json=$open
run report cbmc skipped eee ddd https://run/5
if [ -n "$calls" ]; then
    echo "error: a skipped check called gh: $calls" >&2
    fail=1
fi

# No earlier run: the issue says so instead of naming a range.
issues_json='[]'
run report cbmc failure ccc '' https://run/6
expect_call "No earlier nightly run" "first-ever failure has no range"

# When gh fails the night still finishes, with a warning on the run's summary page.
gh_fails=1
run report cbmc failure ccc bbb https://run/7
case "$err" in
*"::warning"*) ;;
*)
    echo "error: a gh failure printed no warning annotation: $err" >&2
    fail=1
    ;;
esac

exit "$fail"
