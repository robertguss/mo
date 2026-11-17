# Outside-world feasibility preflight

The evidence behind [Mo talks to the outside world](../../../docs/design/outside-world.md). Each
prototype is throwaway. None changes a file under `src/` in the repository, and each `run.sh`
builds what it needs from a clean export of 2b6335c3 or from its own folder.

Host for every number below, unless a line says otherwise: an exe.dev VM, 8 vCPUs of an AMD EPYC
9554P, 31 GiB, Ubuntu 24.04.4, Linux 6.12.113, gcc 13.3.0.

## HTTP written in Mo

[http-in-mo/](http-in-mo/README.md): `bash learnability/phase7/preflight/http-in-mo/run.sh`

- The bar was committed before any measurement: 2.0 s of wall time and 256 MiB of heap for one
  `mo run` that fetches about 1 MiB of JSON and parses it, on the debug build.
- **The wall-time bar is missed: 5.61 s with `Content-Length` and 5.83 s chunked. The heap bar is
  met: 37.0 MiB.**
- The miss is `Json.parse`, which exists today: 109,677,820 of 109,685,165 instructions. HTTP
  parsed in Mo adds 7,345 instructions, or 95,999 when the response is chunked.
- The prototype adds two `Bytes` operations, `slice` and `join`. Without them the same fetch
  takes 14.04 s.
- A 100 KiB response takes 0.63 s. An optimized build takes 2.16 s for 1 MiB.

## Confinement of a child

[confinement/](confinement/README.md): `bash learnability/phase7/preflight/confinement/run.sh NAME`

- A probe makes every attempt a taken-over child might: files outside its folder, an earlier
  child's output, the service's data, TCP, UDP, two kinds of Unix socket, a signal to its parent
  and two reads of its parent's memory.
- Landlock and a seccomp filter together refuse every attempt, on the development host and on a
  GitHub-hosted `ubuntu-24.04` runner, with no privilege and no setup.
- Landlock alone leaves UDP and Unix sockets with a path. A seccomp filter alone leaves every
  file.
- Namespaces confine on the development host and cannot be set up on the runner.
- With no mechanism, a child on the development host can read its parent's memory. The parent
  is `mo serve`.

## The proposed surface, type-checked

[surface/](surface/README.md): `mo check` on two programs

- The spike program and its tests with no network are valid Mo against stand-in modules.
- Nothing in them runs.
- What the checker taught the design is listed in that folder's README.

## What was not tried

- macOS, for confinement or for speed.
- A sanitizer build as the confined child.
- TLS of any kind.
- SQLite through a capability. `src/host/store.c` already runs the same SQLite build for the
  service store.
- A JSON parser that reads text through primitives.
