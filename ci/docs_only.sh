#!/bin/sh
# Decides whether a change touches only files that no check reads.
#
#   ci/docs_only.sh <base>   changed files are `git diff --name-only <base>...HEAD`
#   ci/docs_only.sh -        changed files are read from standard input, one per line
#
# Prints "true" when every changed file is on the list below and there is at least one, and
# "false" otherwise. The hosted workflow skips the six check groups on "true".
#
# The list is an allow-list on purpose: a file that is not named here runs CI. Three documents
# are NOT on it because a check reads them: docs/agent-guide.md and docs/reference/README.md are
# embedded in the binary (ci/embed_docs.py) and ci/check_cli.py checks them, and
# docs/design/library.md is checked against the primitive tables (make library-map). If a check
# starts reading another document, make sure it is not matched here.
set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: ci/docs_only.sh <base> | -" >&2
    exit 2
fi

if [ "$1" = "-" ]; then
    files=$(cat)
else
    files=$(git diff --name-only "$1"...HEAD)
fi

if [ -z "$files" ]; then
    echo false
    exit 0
fi

result=true
while IFS= read -r file; do
    case "$file" in
    docs/design/library.md | docs/agent-guide.md | docs/reference/*) result=false ;;
    docs/status/*.md | docs/design/*.md | docs/ROADMAP.md) ;;
    README.md | AGENTS.md | CLAUDE.md | .github/pull_request_template.md) ;;
    .agents/skills/*.md | learnability/*.md) ;;
    *) result=false ;;
    esac
done <<EOF2
$files
EOF2

echo "$result"
