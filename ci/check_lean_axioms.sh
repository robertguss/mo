#!/bin/sh
# The axiom check for the offline-diagnosis proofs. Run after the Lean build (`make formal`).
#
# `lake build MoCore` already fails when a theorem's axioms differ from its pinned
# `#guard_msgs in #print axioms` output. This script checks the pins themselves, in each file:
#   - every theorem has a pinned `#print axioms`;
#   - every pinned list names only `propext`, `Classical.choice` and `Quot.sound`;
#   - there is no `sorry`, `native_decide` or `axiom` declaration.
# Then the negative control: a copy of Verdict.lean with a `sorry` planted in the proof of
# `verdict_verified_iff` must fail to compile with `sorryAx` reported for that theorem and for a
# theorem that only uses it, must fail the escape-hatch scan, and, with its pin edited to admit
# `sorryAx`, must fail the allowed-axioms scan.
set -eu
files="formal/MoCore/Saved.lean formal/MoCore/Verdict.lean"
allowed="propext Classical.choice Quot.sound"

# Every theorem has a pin.
pins_complete() {
    missing=0
    for name in $(sed -n 's/^theorem \([A-Za-z0-9_]*\).*/\1/p' "$1"); do
        if ! grep -qx "#guard_msgs in #print axioms $name" "$1"; then
            echo "error: $1: theorem $name has no pinned #print axioms" >&2
            missing=1
        fi
    done
    return $missing
}

# Every pinned axiom is allowed.
pins_allowed() {
    bad=0
    for axiom in $(sed -n "s/^info: '.*' depends on axioms: \[\(.*\)\]\$/\1/p" "$1" | tr -d ',' |
        tr ' ' '\n' | sort -u); do
        case " $allowed " in
        *" $axiom "*) ;;
        *)
            echo "error: $1: a pinned #print axioms lists $axiom" >&2
            bad=1
            ;;
        esac
    done
    return $bad
}

# No proof escapes the kernel.
no_escape_hatches() {
    if grep -nwE 'sorry|native_decide' "$1" >&2 || grep -nE '^[[:space:]]*axiom[[:space:]]' "$1" >&2; then
        echo "error: $1: sorry, native_decide or an axiom declaration" >&2
        return 1
    fi
}

for f in $files; do
    pins_complete "$f"
    pins_allowed "$f"
    no_escape_hatches "$f"
done

tree=$(mktemp -d)
trap 'rm -rf "$tree"' EXIT
trap 'exit 1' HUP INT TERM
planted="$tree/Verdict.lean"
# Keep the theorem's statement, through the line ending in `:= by`, and replace its proof.
awk '
    /^theorem verdict_verified_iff / { inside = 1 }
    inside == 1 && /:= by$/ { print; print "  sorry"; inside = 2; planted = 1; next }
    inside == 2 && /^$/ { inside = 0 }
    inside == 2 { next }
    { print }
    END { exit planted ? 0 : 1 }
' formal/MoCore/Verdict.lean >"$planted" || {
    echo 'error: could not plant sorry in verdict_verified_iff in formal/MoCore/Verdict.lean' >&2
    exit 1
}
if (cd formal && lake env lean "$planted") >"$tree/out" 2>&1; then
    echo 'error: Verdict.lean with a planted sorry compiled' >&2
    exit 1
fi
for name in verdict_verified_iff verdict_not_verified_of_not_reproduced; do
    if ! grep -q "^+ info: 'MoCore.Verdict.$name' depends on axioms: \[.*sorryAx.*\]" "$tree/out"; then
        echo "error: the planted sorry was not reported for $name; lean said:" >&2
        cat "$tree/out" >&2
        exit 1
    fi
done
if no_escape_hatches "$planted" 2>/dev/null; then
    echo 'error: the escape-hatch scan missed the planted sorry' >&2
    exit 1
fi
sed "s/^\(info: 'MoCore.Verdict.verdict_verified_iff' depends on axioms: \[.*\)\]\$/\1, sorryAx]/" \
    "$planted" >"$tree/repinned.lean"
if pins_allowed "$tree/repinned.lean" 2>/dev/null; then
    echo 'error: the allowed-axioms scan accepted a pin listing sorryAx' >&2
    exit 1
fi
echo "Lean axioms: every theorem in $(echo $files | wc -w) files pins only $allowed;" \
    'a planted sorry fails the build and both scans'
