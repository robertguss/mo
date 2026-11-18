---
title: A tracked Mo program must end by itself when run with no arguments
date: 2026-11-17
category: workflow-issues
module: ci/check_format.py
problem_type: workflow_issue
component: testing_framework
severity: medium
applies_when:
  - A new `.mo` file is committed anywhere in the repository, including prototypes under `learnability/`
  - The program expects arguments, input or a server to be running
  - Only `bash ci/check.sh gcc` is run before the pull request
tags: [drivers, format-check, prototype, learnability, timeout, nightly, mo-run]
---

# A tracked Mo program must end by itself when run with no arguments

## Context

The Phase 7 design step committed a prototype, `learnability/phase7/preflight/http-in-mo/today/fetch.mo`,
that took a port and a path as arguments and read from a connection. `bash ci/check.sh gcc`
passed. The `drivers` group then failed in "Every tracked Mo source", a check whose name says
nothing about prototypes, with Python's `TimeoutExpired` for `mo run` on that file after 30
seconds.

`ci/check_format.py` is the cause. To show that `mo format` changes no behaviour, `audit()`
copies every tracked file to a temporary folder and, for every tracked `.mo` file that passes
`mo check`, runs `mo run FILE` before formatting and again after, then compares the two
results. `invoke()` gives each run no arguments, no input and 30 seconds.

With no arguments the prototype read an empty port, opened a file instead of a connection, and
waited in its read loop.

## Guidance

Every tracked `.mo` file with a `main` must, when run with no arguments and no input:

- end well inside 30 seconds, and
- give the same output and exit status every time, because the two runs are compared.

For a program that needs arguments, match the argument list first and print a usage line:

```mo
fn main(args: Args, console: Console, files: Files) : Int
  case args.list
    [port, path]: report(console, files, port, path)
    _:
      console.error("usage: fetch PORT PATH")
      2
  end
end
```

Before committing a new `.mo` file, run it once by hand with no arguments, or run the check:

```sh
make CC=gcc
python3 ci/check_format.py build/gcc/debug/mo
```

## Why This Matters

`AGENTS.md` asks for the `gcc` group only before a pull request, and the `drivers` group runs
in the nightly. A prototype that waits for input therefore passes every check its author runs
and turns the nightly red a day later, in a check about formatting, and a red nightly comes
before any feature.

## When to Apply

Any change that adds a `.mo` file: samples, fixtures, benchmark programs and throwaway
prototypes alike. A file that does not pass `mo check`, or has no `main`, is not run.
