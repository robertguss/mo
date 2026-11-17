#!/usr/bin/env bash
set -uo pipefail

here=$(cd "$(dirname "$0")" && pwd)
name=${1:?give the host a name, such as dev-host}
work=$(mktemp -d)
mkdir -p "$here/out"
gcc -O1 -Wall -Wextra -Wno-format-truncation -o "$work/confine" "$here/confine.c" || exit 3

show() {
    printf '%s: %s\n' "$1" "$(cat "$2" 2>/dev/null || echo 'not present')"
}
{
    echo "host: $name"
    echo "kernel: $(uname -r)"
    echo "system: $(. /etc/os-release && echo "$PRETTY_NAME")"
    echo "compiler: $(gcc --version | head -1)"
    show "security modules" /sys/kernel/security/lsm
    show "kernel.unprivileged_userns_clone" /proc/sys/kernel/unprivileged_userns_clone
    show "kernel.apparmor_restrict_unprivileged_userns" /proc/sys/kernel/apparmor_restrict_unprivileged_userns
    show "kernel.yama.ptrace_scope" /proc/sys/kernel/yama/ptrace_scope
    show "seccomp actions" /proc/sys/kernel/seccomp/actions_avail
    for mechanism in none landlock seccomp landlock+seccomp namespaces; do
        echo
        "$work/confine" "$mechanism"
        echo "exit: $?"
    done
} >"$here/out/$name.txt" 2>&1
rm -rf "$work"
cat "$here/out/$name.txt"
