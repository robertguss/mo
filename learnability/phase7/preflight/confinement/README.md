# Confinement of a child: which mechanism works on which host

A throwaway probe, built on commit 2b6335c3, that answers one planning question: which
operating-system mechanism can confine the compile, listing and check children of `mo serve`, on
the development host and on the nightly's host?

Verdict: Landlock and a seccomp filter together confine the child on both hosts, with no
privilege and no setup. Either one alone leaves a gap. Namespaces confine on the development host
and cannot be set up on the nightly's host.

## Files

| File | What it is |
| --- | --- |
| `confine.c` | The probe. `confine MECHANISM` sets up what a child may and may not reach, forks, applies the mechanism where `spawn()` in `src/host/compile.c` would, and executes itself with `--probe`. |
| `run.sh` | Compiles the probe and runs it under every mechanism. `bash run.sh NAME` writes `out/NAME.txt`. |
| `out/dev-host.txt` | The development host: an exe.dev VM, Ubuntu 24.04.4, Linux 6.12.113. |
| `out/ubuntu-24.04-runner.txt` | A GitHub-hosted `ubuntu-24.04` runner, the image the nightly uses: Ubuntu 24.04.5, Linux 7.0.0-1017-azure. From [run 44163889813](https://github.com/robertguss/mo/actions/runs/44163889813), on a scratch branch that held this folder and one workflow file. |

## What the probe sets up

The parent makes a folder under `/tmp` with five folders in it, and four sockets:

| Stands for | The child |
| --- | --- |
| `scratch/`, its own folder | reads and writes it |
| `earlier/image.bc`, what an earlier child wrote | reads it, must not change it |
| `lib/list.mo`, the library folder | reads it |
| `outside/secret`, any other file of the user's | must not reach it |
| `data/store.sqlite`, the service's data folder | must not reach it |
| A TCP listener and a UDP socket on loopback | must not reach them |
| A Unix socket with a path, and one with an abstract name | must not reach them |

The child is the probe itself, executed through `/proc/self/exe` with an empty environment after
the mechanism is applied. It is linked dynamically, as `mo` is, so it shows that the loader and
the C library still load. It also reads `/proc/self/exe`, as every `mo` command except `check`
does, and opens `/dev/null`.

Three attempts go beyond the list in the plan: reading `/etc/passwd`, reading the parent's memory
with `process_vm_readv`, and opening `/proc/PARENT/mem`. The parent is `mo serve`, which holds
the upgrade and approval tokens in memory.

## Results

"Refused" means the operating system returned an error. "Allowed" means the attempt worked.

| Attempt | No mechanism | Landlock | seccomp | Landlock and seccomp | Namespaces |
| --- | --- | --- | --- | --- | --- |
| Change an earlier child's output | allowed | refused | allowed | refused | refused |
| Open a file outside | allowed | refused | allowed | refused | refused |
| Create a file outside | allowed | refused | allowed | refused | refused |
| Read the service's data folder | allowed | refused | allowed | refused | refused |
| Read `/etc/passwd` | allowed | refused | allowed | refused | refused |
| Connect over TCP to loopback | allowed | refused | refused | refused | refused |
| Send a UDP datagram | allowed | **allowed** | refused | refused | refused |
| Connect to a Unix socket by path | allowed | **allowed** | refused | refused | refused |
| Connect to an abstract Unix socket | allowed | refused | refused | refused | refused |
| Signal its parent | allowed | refused | refused | refused | refused |
| Read its parent's memory | allowed on the development host, refused on the runner | refused | refused | refused | refused |
| Open its parent's memory file | allowed on the development host, refused on the runner | refused | **allowed on the development host** | refused | refused |
| Everything the child needs | works | works | works | works | works |

The two hosts agree on every cell except where the table says so, and except for the last
column: on the runner the namespaces could not be set up at all.

| Host | Landlock | seccomp filter | Namespaces |
| --- | --- | --- | --- |
| Development host | Available, ABI 6 | Available | Available: every attempt refused |
| `ubuntu-24.04` runner | Available, ABI 8 | Available | Unavailable: writing `/proc/self/setgroups` gives "Permission denied". The image sets `kernel.apparmor_restrict_unprivileged_userns` to 1. |

## What each mechanism is in the probe

- **Landlock.** A ruleset that handles every file access right the kernel's ABI offers, TCP bind
  and connect from ABI 4, and the scopes for abstract Unix sockets and signals from ABI 6. Rules
  allow reading and executing `/usr`, `/lib`, `/lib64`, `/etc/ld.so.cache` and the executable;
  reading `lib/` and `earlier/`; reading and writing `scratch/` and `/dev/null`. Landlock also
  stops the child tracing a process outside its own domain, which is what refuses the two reads
  of the parent's memory.
- **seccomp.** A filter that refuses, with "Operation not permitted", the system calls for
  sockets, signals to another process, tracing, reading another process's memory, `io_uring`,
  namespaces and mounting. It kills the process on a foreign architecture or an x32 call number.
  `tgkill` is allowed only at the child's own process, because `abort()` uses it.
- **Namespaces.** New user, mount, network, IPC and process-id namespaces, and a new root that
  holds only what the child may reach, with `earlier/`, `lib/` and the system folders read-only.

## What this does not show

- Nothing here runs `mo`. The probe stands in for the child. A sanitizer build, which reads more
  under `/proc/self`, was not tried.
- Landlock before ABI 6 does not scope abstract Unix sockets or signals, and before ABI 4 it does
  not handle TCP. Both hosts are past that. With the seccomp filter in place those gaps are
  closed whatever the ABI, because the filter refuses the system calls.
- The seccomp filter is a list of refused calls. A list of allowed calls would be stricter, and
  was not tried, because it must name every call the C library and a sanitizer make.
- macOS has neither mechanism. No candidate was tried there.
- The probe opens the parent's memory file. It does not show what could be read from it.
