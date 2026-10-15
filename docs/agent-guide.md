# Write a finished Mo program

This page is the procedure for a finished Mo program. Correct Mo is Language Reference v0.1. `mo guide --reference` prints that reference. Section 13.4 says this guide covers how to work with the `mo` command, a small complete program with its tests, and when a task is done.

## Know this build

The language headings below use the present tense of the reference.

`mo check` on this build runs a staged checker. A body form it has not implemented is unfinished. The code is E1103. The headline is "this body rule is not implemented by the staged checker". A nonempty `examples` or `promise` block is one of those forms. On the program below, check also reports that code for `console.print`. With the `examples` block removed, it reports the same code for the interpolated text. The same code covers other forms the reference requires, including `or` on an `Option` and a dot call such as `get`.

E1103 means this toolchain has not implemented that form. If `mo check` reports E1103 and the reference requires the form, leave the form in. A `test` block is not that diagnostic. This build does not look at tests, so deleting a `test` does not change the check, and exit 0 does not mean a test ran.

`mo run` exits 1 with E1400. The headline is "this build cannot run programs yet". `mo test` exits 1 with E1400. The headline is "this build cannot run or list tests yet". `mo fix` exits 1 with E1400. The headline is "this build cannot apply fixes yet". `--emit` exits 1 with E1400. The headline is "this build cannot emit Core or bytecode yet".

E1400 means the command does no work on this build. Leave the program as the reference requires.

This build runs `mo format`. `mo format` rewrites the files it is given.

This build has no `lib/` directory. It loads no library modules from that folder. Section 10.5 names what is always in scope. A call this checker has not implemented is still E1103.

Exit 0 means the staged checker accepted the forms it implements.

## Write this program

Use this shape. Keep the `examples` block, the interpolated text, and `console.print`. This build reports E1103 for those forms.

```mo
## Greets the name given on the command line, or the world.
module Greet

fn greeting(name: Option(Text)) : Text
  examples
    greeting(Some("Ada")) == "Hello, Ada!"
    greeting(None) == "Hello, world!"
  end

  "Hello, #{name or "world"}!"
end

fn main(args: Args, console: Console)
  console.print(greeting(args.list.first))
end

test "only the first argument is used"
  console = Console.fake()
  main(Args.of(["Ada", "Grace"]), console)
  assert console.printed == ["Hello, Ada!"]
end
```

The `##` comment documents the module, and the module line names `Greet`. Sections 1 and 2.2.

`greeting` types its parameter and its result. Inside a body, types are inferred from all their uses in it. The body's value is the value of its last line. Section 5.1.

`name` is an `Option`. `Some` and `None` are its cases, and `name or "world"` gives the value inside `Some`, or `"world"` for `None`. Sections 3 and 8.1.

`"Hello, #{name or "world"}!"` inserts the text form of that value. Section 3.4.

`main` lists the capabilities the program needs, here `Args` and `Console`. Sections 11.1 and 11.2.

`examples` sits at the top of the body, then a blank line, then the code. Sections 12 and 12.1.

The `test` uses `Console.fake()`, `Args.of`, and `console.printed`. Sections 12.3 and 12.4.

`args.list.first` calls `first` with no other arguments. This file has no `promise`. Sections 5.4 and 12.2.

## Follow these rules

Write these forms. The section number states the rule.

- Where another language writes `items[i]` or `map[key]`, write `items.get(i)` or `map.get(key)`. `get` returns an `Option`. Sections 3.7 and 3.8.
- Bind a name once with `name = value`. A name is visible from its binding to the end of its block. To assign it again, declare `var name = value`. Sections 6.1 and 6.2.
- Add an element with `items = items.append(x)`, and declare `var items` when you assign that name again. Sections 3.7 and 6.2.
- Mo has no exceptions, and there is no "unwrap". Return `Error(...)`, pass it on with `try`, and handle it with `case`. Where a failure truly cannot happen, write `stop` and the reason. Sections 8.2, 8.3, and 8.4.
- Cover every possible value in a `case`. On a choice, a whole arm cannot be `_` or a name, so list the cases. Inside a pattern, `_` or a name may stand for anything. Section 7.2.
- Pass a capability in only as a parameter. Make a fake only in tests, examples, and promises. Sections 11 and 12.4.
- Declare a `struct` or an `enum`. Records have no methods. Put functions in that module. Sections 4 and 5.4.
- To state that a function holds for every input, write a `promise` with a sentence, a body, an `allows` line, and a `forbids` line. Section 12.2.
- When another module needs a name, list it with `expose` and bring the module in with `use`. Section 10.

The program `Shout` in section 1 has no `examples` and no `promise`.

Pass function arguments by position. Only records and cases are built with named fields. Section 5.2.

Read the section for the rule.

- Section 1 is already in the sample as a module and `main`, and indentation carries no meaning.
- Section 2 is files, comments, names, and lines. The sample's opening comment is section 2.2.
- Section 3 is values and types, including the sample's text and lists. Mo never converts between types silently.
- Section 4 is a `struct` or an `enum`.
- Section 5 is recursion, generic functions, and functions as values. There are no default or optional parameters.
- Section 6 is a name bound once, and `var` for a later assignment.
- Section 7 is `if`, `case`, `is`, and loops.
- Section 8 is `Option` and `Result`. The sample uses `Option` and `or`.
- Section 9 is the five abilities, `where`, and `to_text` on a type with `Text`.
- Section 10 is `expose`, `use`, and constants. The sample is one module.
- Section 11 is the capabilities besides `Args` and `Console`, and narrowing a capability.
- Section 12 is examples, promises, and tests. The sample has examples and a test, and no promise.
- Section 13 is commands and done, under the headings below.
- Section 14 is the catalog. Call library functions as that section lists them, through the type.

Section 15 names later work, and it changes no current rule.

The executable grammar in `grammar/mo.y` is the authority. Appendix B summarizes it.

## Run these commands

Each command is given the file of the program's main module. `mo check FILE` reports the program's errors, `mo run FILE [ARGS...]` runs it with the arguments after the file's name, and `mo test FILE` checks its examples, promises and tests. `mo format` and `mo fix` rewrite the files they are given.

`mo guide` needs no file. It prints this page. `mo guide --reference` prints the reference.

`mo` exits with status 0 when everything passed, 1 when the program has errors or an example, promise or test fails, and 2 for a mistake in the command itself. Once the program runs, `mo run` exits with the program's own status, or 70 after a bug. Section 11.2 states the program's own status. Section 13.2 states that bug.

Section 13.4 names the selection flags and the limits.

## Work in this loop

Follow these steps for a complete toolchain.

1. Write the program. When a function has `examples` or a `promise`, put each block at the top of the body, then a blank line, then the code. Add a `test` in the same file when you check a scenario. Section 12.
2. Run `mo format` on those files.
3. Run `mo check FILE`. Fix language errors. Keep a diagnostic that Know this build names.
4. When `mo check` offers exactly one fix, run `mo fix FILE`. Appendix A says `mo fix` applies a fix when there is only one.
5. Run `mo test FILE` until the examples, promises, and tests pass.
6. Run `mo run FILE` only when the task needs a running program.

If a command exits 1 for a reason that Know this build names, keep the source that the reference requires, and continue. Section 13.4 describes `--json`.

## Stop when the task is done

The task is done when the program is the v0.1 Mo the reference describes for what it computes, including the specs and tests that behavior needs, and you have run the loop steps this build implements.

On a complete toolchain, `mo format`, `mo check`, and `mo test` exit 0. Run `mo run` when the task is a running program.

On this build, `mo check` exit 0 is not the bar. Accept a remaining diagnostic only when Know this build names it. Deleting a reference form to silence E1103 leaves the task undone. Skipping tests because a command returns E1400 leaves the task undone.

Where the reference says nothing, do not add a rule, and do not fill that silence from the architecture or from `formal/TRANSLATION.md`.

Section 12 shows the form of a spec. When the task is a program, read section 11.2 for `main`'s result and the program's exit status.
