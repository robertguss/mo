import pathlib
import shutil
import subprocess
import tempfile

from cli_common import (
    SERVE_COUNTER, SERVE_OPERATOR_NAMES, SERVE_UPGRADE_NAMES, root, run,
)

flags = {
    "--json": ([], {"check", "test", "replay", "query", "history", "push", "diff"}),
    "--list": ([], {"test"}),
    "--reference": ([], {"guide"}),
    "--only": (["Main.test.1"], {"test"}),
    "--in": (["Main"], {"test"}),
    "--max-stack": (["1MiB"], {"run", "test", "query"}),
    "--max-depth": (["100"], {"run", "test", "query"}),
    "--max-heap": (["1GiB"], {"run", "test", "query"}),
    "--calls": (["30"], {"test"}),
    "--seconds": (["3"], {"test"}),
    "--mutants": ([], {"test"}),
    "--lib": (["lib"], {"check", "run", "test", "fix", "emit", "serve", "replay", "query", "push", "diff", "install"}),
    "--root": (["."], {"run", "install"}),
    "--state-dir": (["."], {"run", "install"}),
    "--name": (["tool"], {"install"}),
    "--bin-dir": (["bin"], {"install"}),
    "--port": (["0"], {"serve"}),
    "--bind": (["127.0.0.1"], {"serve"}),
    "--workers": (["4"], {"serve"}),
    "--watch": ([], {"serve"}),
    "--data": (["state"], {"serve", "replay", "query"}),
    "--allow-host": (["queue.example"], {"serve"}),
    "--admin-socket": (["admin.sock"], {"serve", "push", "query"}),
    "--unconfined": ([], {"serve"}),
    "--emit=core": ([], {"check"}),
    "--emit=bytecode": ([], {"check"}),
    "--failure": (["1"], {"replay", "query"}),
    "--export": (["failure.moex"], {"replay", "query"}),
    "--fix": (["revised/main.mo"], {"replay"}),
    "--recover": ([], {"replay", "query", "history"}),
    "--max-work": (["1000"], {"query"}),
}


def guide_claims(binary):
    """`mo guide` tells agents what this build does; each claim is checked against the build.

    When one of these fails, the build changed: update docs/agent-guide.md to match, then this
    test. Never edit only the test."""
    guide = (root / "docs/agent-guide.md").read_text(encoding="utf-8")
    # The live operator driver exercises these guide claims through the real admin socket.
    for name in SERVE_OPERATOR_NAMES:
        assert f"`{name}`" in guide
    # ci/check_offline.py exercises the offline commands' claims against real data folders.
    assert ("On the service host, `mo history`, `mo replay` and `mo query` read a stopped data "
            "folder or an artifact without starting the service, as Diagnose a failure offline "
            "says.") in guide
    assert "mo fetch --admin-socket PATH --export failure ID --out failure.moex" in guide
    # ci/check_upgrade_live.py and ci/check_upgrade_remote.py exercise these claims through
    # mo serve, mo push, mo query and mo fetch over --admin-socket.
    for name in SERVE_UPGRADE_NAMES:
        assert f"`{name}`" in guide
    for sentence in ("Exit 1 means a refusal or client failure; exit 2 means invalid command usage or base digest.",
                     "Public HTTP (`--port N`) exposes only the tools declared in `service ... end`.",
                     "Only the nine operator views are read-only; the three upgrade tools carry "
                     "`readOnlyHint: false`, and commit carries `destructiveHint: true`.",
                     "When `--base` is omitted, `mo push` reads the active program's source digest "
                     "from `mo-overview`;",
                     "Pushing the active program again reports `already active` and changes "
                     "nothing",
                     "Where the host can confine, `--unconfined` changes nothing.",
                     "a new GET replaces it.",
                     "fetch an export with `mo fetch`, and replay or query a fetched export with "
                     "the same `mo` build and the deployed source."):
        assert sentence in guide, f"docs/agent-guide.md no longer says: {sentence}"
    assert ("A wrong version that stops is kept out of the rate, and so is one that does not check "
            "or changes a digest, unless a `counted` file in `DIR` lists it: a listed one stays in "
            "the rate as not killed.") in guide
    assert ("`mo diff [OLD] NEW` checks how a program changed from `OLD` (or Git `HEAD` when only "
            "`NEW` is given) to `NEW`: capability authority (including `Files.within`, "
            "`ReadFiles.within`, `Files.read_only`, `Network.allow_hosts`, `Command.within` and "
            "`Command.allow_programs` narrowings), exposed "
            "signatures and service tools, saved state schema (`migration_required`), and "
            "behavioral contracts by running `OLD`'s promises, service promises and examples "
            "against `NEW`.") in guide
    blocks = guide.split("```mo\n")
    assert len(blocks) == 3, "the guide has two Mo programs: the API call and its example"
    program = blocks[2].split("```\n", 1)[0]

    def says(*sentences):
        for sentence in sentences:
            assert sentence in guide, f"docs/agent-guide.md no longer says: {sentence}"

    def refuses(args, code, headline, claim):
        # The claim is the guide's own sentence, so it must name this code and this headline.
        assert f"E{code}" in claim and f'"{headline}"' in claim, claim
        says(claim)
        result = run(binary, args, 1)
        assert not result.stdout, (args, result.stdout)
        assert f"error E{code}: {headline}".encode() in result.stderr, (args, result.stderr)

    says("Each mutant that survives is an error, E1439, \"a mutant survived: no check noticed this "
         "change\"",
         "the line says `mutants: did not run: a check did not pass`",
         "`mo test FILE --mutants` is an opt-in diagnostic pass that runs every check with no time "
         "limit and then tries mutants of the program (reference 12.6).")
    plain_ok = run(binary, ["test", "tests/errors/e1439/main.mo"], 0)
    assert not plain_ok.stderr and b"mutants:" not in plain_ok.stdout
    survivor = run(binary, ["test", "--mutants", "tests/errors/e1439/main.mo"], 1)
    assert b"error E1439: a mutant survived: no check noticed this change" in survivor.stderr
    says("`mo replay`, `mo query` and `mo history` exit 0 when the requested result holds, 1 when "
         "it does not or a check refused, and 2 for a mistake in the command")
    assert ("The function is not generic (E1410), holds no capability or function in its "
            "parameters or result (E1411), takes the main service's state type first (E1412) and "
            "then exactly the inputs above (E1413).") in " ".join(guide.split())
    with tempfile.TemporaryDirectory() as empty:
        result = run(binary, ["history", empty], 1)
        assert result.stderr == f"mo history: no saved data in {empty}\n".encode(), result
    for code, function in (("1410", "size"), ("1411", "peek"), ("1412", "double"), ("1413", "plus")):
        result = run(binary, ["query", f"tests/errors/e{code}/main.mo", function, "--export",
                              "tests/errors/missing.moex"], 1)
        assert result.stderr.startswith(f"error E{code}: ".encode()) and not result.stdout, result
    result = run(binary, ["query", "tests/errors/e1413/main.mo", "nothing", "--export",
                          "tests/errors/missing.moex"], 2)
    assert result.stderr == b"mo query: no function of that name in that module\n", result
    says("This build runs `mo check`, `mo run`, `mo test`, `mo format`, `mo fix`, `mo guide`, `mo emit`, `mo serve`, `mo fetch`, `mo push`, `mo replay`, `mo query`, `mo history`, `mo diff` and `mo install`.",
         "`mo run [--root DIR] [--state-dir DIR] FILE [ARGS...]` grants `DIR` (`.` by default) as "
         "the starting folder for `Files`, `ReadFiles` and `Command` (and `Sqlite` unless "
         "`--state-dir` sets a separate folder for `Sqlite`), refusing an unreadable folder with "
         "E1404 while keeping relative-path containment inside each folder.",
         "`mo install FILE [--name NAME] [--bin-dir DIR] [--root DIR] [--state-dir DIR]` compiles "
         "`FILE` to verified bytecode under `DIR/.mo-libexec/<name>.bc` (`~/.local/bin` by "
         "default) and writes an executable launcher `DIR/<name>` (accepting leading `--mo-root` "
         "and `--mo-state-dir` overrides at run time).",
         "The program below checks with no errors, runs, and passes its examples and its test.",
         "E1103 and E1150 mean this toolchain has not implemented that form.",
         "`mo run` and `mo test` report the same code for the same form.",
         "accept a remaining diagnostic only when Know this build names it: E1103 or E1150 on a "
         "form the reference requires and offers no other way to write the same behaviour, or "
         "E1400 on a promise whose inputs `mo test` cannot build.",
         "If the reference offers another way to write the same behaviour from forms that "
         "compile, use that instead.",
         "Deleting a required spec, promise or capability to silence a diagnostic leaves the "
         "task undone.")
    says("This build checks, formats, fixes and serves `service ... end` blocks (reference 11.5).",
         "`mo check --emit=bytecode FILE` prints the program's bytecode as indexed text, with a "
         "`services` section holding one descriptor for each module that declares a service, "
         "including imported modules.",
         "It works on a module without `main`.",
         "`mo emit FILE -o OUT` writes the same descriptors and accepts a service without `main`.",
         "Neither command evaluates a service's start value or reads its token, and `mo run` "
         "still does not start a service (E1408).",
         "`mo check --emit=core FILE` shows the start value and timer intervals as ordinary "
         "functions but no service registration, so Core output alone cannot reconstruct a service.",
         "`mo test` never reads a service's token, and starts a service on its own only in the "
         "service's recovery check, below.",
         "Tests and examples can explicitly start one with `Simulation.start(Module, at)`, "
         "send registered tools with `Simulation.send(handle, Module.tool, args...)`, "
         "advance its fake clock, and crash/restart through the real log and snapshot path "
         "backed by an in-memory store.",
         "`send` returns the complete declared handler result inside another `Result`; "
         "an application `Error` is still inside `Ok`.",
         "Helpers may receive the handle.",
         "A promise whose inputs include a `Simulation` is E1400; promises over a service's "
         "sequences of steps are deferred.",
         "Importing a service module does not start it.",
         "`mo fix` applies neither alternative.",
         "A `Result(State, E)` tool receives no fixes.",
         "Functions on the module's `expose` line receive no fixes because callers in other "
         "modules cannot be updated.",
         "Functions whose names end in `?` receive no fixes because a `?` name must still "
         "return `Bool`.")
    refuses(['run', 'tests/errors/e1408/main.mo'], '1408', 'mo run does not start services yet',
            '`mo run` on a service entry module still refuses with E1408, '
            '"mo run does not start services yet".')
    refuses(['check', 'tests/errors/e0101/main.mo'], '0101',
            'a service tool returns only its state',
            'A service tool that returns exactly `State` or `Result(State, E)` gets E0101, '
            '"a service tool returns only its state".')
    refuses(['check', 'tests/errors/e0100-generic/main.mo'], '0100',
            'a service handler is generic',
            'A selected generic handler gets E0100, "a service handler is generic".')
    simulation = run(binary, ['test', '--calls', '10', 'tests/lower-source/service-simulation/main.mo'],
                     0)
    assert not simulation.stderr and simulation.stdout.endswith(b"11 passed, 0 failed, 0 stopped\n")
    refused = run(binary, ['test', 'tests/errors/e1400-simulation/main.mo'], 1)
    assert b"error E1400: " in refused.stderr and b"promise" in refused.stderr, refused.stderr
    says("`mo test` runs one recovery check for each service in the program, named after its "
         "module, such as `Counter.service.recovery` (reference 12.5).",
         "Its author writes nothing for it.",
         "`--list` prints it after its module's other checks, and `--only` and `--in` select it.",
         "A service's own promises (reference 12.2) are checked on the same sequences.",
         "Each is named after its position, such as `Counter.service.promise.1`, listed just "
         "before the recovery check, and judged after every step it covers, restarts included.",
         "Every service check runs its whole plan of 200 sequences, which `--calls` caps; "
         "`--seconds` does not apply.",
         "A promise body that runs past `MO_LIMIT_SERVICE_WORK` stops with that bug.",
         "For `counter.mo` under Run a service, `mo test counter.mo` prints "
         "`passed Counter.service.promise.1  counter.mo:8:3`, "
         "`passed Counter.service.promise.2  counter.mo:13:3` and "
         "`passed Counter.service.recovery  counter.mo:3:1`, then `3 passed, 0 failed, 0 stopped`.",
         "When the start value stops, the check stops with that bug.",
         "`mo test FILE` checks its examples, promises, tests and recovery checks.",
         "1 when the program has errors or an example, promise, test or recovery check fails")
    with tempfile.TemporaryDirectory(prefix="mo-guide-recovery-") as directory:
        (pathlib.Path(directory) / "counter.mo").write_text(SERVE_COUNTER)
        counter = subprocess.run([str(binary), "test", "counter.mo"], cwd=directory,
                                 capture_output=True, timeout=60)
        assert counter.returncode == 0 and not counter.stderr, counter
        assert counter.stdout == (
            b"passed Counter.service.promise.1  counter.mo:8:3\n"
            b"passed Counter.service.promise.2  counter.mo:13:3\n"
            b"passed Counter.service.recovery  counter.mo:3:1\n"
            b"3 passed, 0 failed, 0 stopped\n"), counter.stdout
        listed = subprocess.run([str(binary), "test", "--list", "counter.mo"], cwd=directory,
                                capture_output=True, timeout=30)
        assert listed.stdout.decode().splitlines() == [
            "Counter.service.promise.1", "Counter.service.promise.2",
            "Counter.service.recovery"], listed
        mutant_listed = subprocess.run([str(binary), "test", "--mutants", "--list", "counter.mo"],
                                       cwd=directory, capture_output=True, timeout=30)
        assert mutant_listed.stdout.decode().splitlines() == [
            "Counter.service.promise.1", "Counter.service.promise.2", "Counter.service.recovery",
            "mutant counter.mo:20:10 Counter.add: arithmetic, `+` to `-`",
            "mutant counter.mo:20:26 Counter.add: arithmetic, `+` to `-`",
            "function Counter.add: 2 mutants", "function Counter.read: 0 mutants"], mutant_listed
        run(binary, ["format", str(pathlib.Path(directory) / "counter.mo")], 0)
        assert (pathlib.Path(directory) / "counter.mo").read_text() == SERVE_COUNTER
    with tempfile.TemporaryDirectory(prefix="mo-guide-") as directory:
        folder = pathlib.Path(directory)
        greet = folder / "greet.mo"
        greet.write_text(program, encoding="utf-8")
        result = run(binary, ["check", str(greet)], 0)
        assert not result.stdout and not result.stderr, result
        result = run(binary, ["test", str(greet)], 0)
        assert not result.stderr and result.stdout.endswith(b"3 passed, 0 failed, 0 stopped\n")
        assert run(binary, ["run", str(greet), "Ada", "Grace"], 0).stdout == b"Hello, Ada!\n"
        assert run(binary, ["run", str(greet)], 0).stdout == b"Hello, world!\n"
        result = run(binary, ["format", str(greet)], 0)
        assert not result.stdout and not result.stderr
        assert greet.read_text(encoding="utf-8") == program, "the guide's program is formatted"

        result = run(binary, ["fix", str(greet)], 0)
        assert not result.stdout and not result.stderr
        assert greet.read_text(encoding="utf-8") == program
        says("`mo fix FILE` makes each edit that `mo check` shows as an error's only fix, checks "
             "again, and prints the errors that are left, as `mo check` does. It rewrites only "
             "the files it is given.",
             "`mo format --stdin` formats standard input to standard output for editors without "
             "touching files on disk.",
             "`mo format` and `mo fix` rewrite the files they are given.")
        says("`mo emit FILE -o OUT` checks the program as `mo check` does, then writes its bytecode "
             "to `OUT`. `-o` is required, and `OUT` is never a source file of the program. A module "
             "with neither `main` nor a service is E0079, as for `mo run`, and a file that cannot be written is "
             "E1404. `mo run OUT [ARGS...]` loads, verifies and runs an emitted bytecode file directly without "
             "source files; an unreadable or invalid bytecode file is E1404, a service entry module is E1408, "
             "and a module without `main` is E0079.")
        unused = folder / "unused.mo"
        unused.write_text("module Unused\n\nfn one(x: Int) : Int\n  1\nend\n", encoding="utf-8")
        assert b"Fix: " in run(binary, ["check", str(unused)], 1).stderr
        result = run(binary, ["fix", str(unused)], 0)
        assert not result.stdout and not result.stderr
        assert "fn one(_x: Int)" in unused.read_text(encoding="utf-8")

        # The rules first retries missed (MO-137): each mistake's error names the rule's fix.
        says("To assign it again, declare `var name = value` where it is first bound, even when "
             "the assignment is inside a loop or a branch.",
             "`+` adds numbers and does not join texts.",
             "There is no `len` or `length`.")
        counted = folder / "counted.mo"
        counted.write_text("module Counted\n\nfn count(items: List(Int)) : Int\n  total = 0\n"
                           "  for _ in items\n    total += 1\n  end\n  total\nend\n",
                           encoding="utf-8")
        assert b"declare it with var where it is first bound" in \
            run(binary, ["check", str(counted)], 1).stderr
        result = run(binary, ["fix", str(counted)], 0)
        assert not result.stdout and not result.stderr
        assert "  var total = 0\n" in counted.read_text(encoding="utf-8")
        joined = folder / "joined.mo"
        joined.write_text('module Joined\n\nfn join(a: Text, b: Text) : Text\n  a + b\nend\n',
                          encoding="utf-8")
        assert b"+ doesn't join texts" in run(binary, ["check", str(joined)], 1).stderr
        sized = folder / "sized.mo"
        sized.write_text("module Sized\n\nfn size(items: List(Int)) : Int\n  len(items)\nend\n",
                         encoding="utf-8")
        assert b"list.size" in run(binary, ["check", str(sized)], 1).stderr

        # Round 2's next-attempt misses were errors left unedited before a re-run (D296).
        says("It reports each error it finds, in source order. Fix every error it reports before "
             "you run `mo check` again, not only the first.",
             "Run `mo test FILE` only once `mo check` reports no errors but those Know this build "
             "names. While the program has other errors, `mo test` runs nothing and prints the "
             "same errors `mo check` does.")
        several = folder / "several.mo"
        several.write_text("module Several\n\nfn count() : Int\n  total = 0\n  total = 1\n  total\nend\n\n"
                           "fn join(a: Text, b: Text) : Text\n  a + b\nend\n\n"
                           "fn size(items: List(Int)) : Int\n  len(items)\nend\n\n"
                           'test "never runs"\n  assert size([1]) == 1\nend\n', encoding="utf-8")
        checked = run(binary, ["check", str(several)], 1)
        codes = [line.split(b":")[0] for line in checked.stderr.splitlines()
                 if line.startswith(b"error E")]
        assert codes == [b"error E0051", b"error E0075", b"error E0050"], checked.stderr
        tested = run(binary, ["test", str(several)], 1)
        assert not tested.stdout and tested.stderr == checked.stderr, tested

        # Each unfinished form gives the same code from check, run and test.
        unfinished = folder / "unfinished.mo"
        unfinished.write_text("module Unfinished\n\nanswer = invoke(fn() 13 end)\n\n"
                              "fn invoke(action: fn() : Int) : Int\n  action()\nend\n\n"
                              "fn main() : Int\n  answer\nend\n", encoding="utf-8")
        deferred = folder / "deferred.mo"
        deferred.write_text('module Deferred\nhandler = fn() base end\nbase = 13\n'
                            'fn main() : Int\nhandler()\nend\n'
                            'test "reads base"\nassert handler() == 13\nend\n', encoding="utf-8")
        qualified = [folder / "compared.mo", folder / "texted.mo"]
        for path, call in zip(qualified, ('Text.compare("a", "b")', "List.to_text([1])")):
            path.write_text(f"module {path.stem.title()}\n\nfn main(console: Console)\n"
                            f'  console.print("#{{{call}}}")\nend\n', encoding="utf-8")
        receivers = folder / "receivers.mo"
        receivers.write_text("module Receivers\n\nfn main(console: Console)\n"
                             '  console.print("#{Less.compare(Greater)} #{Folder.to_text}")\nend\n',
                             encoding="utf-8")
        says("`T.to_text(x)` and `T.compare(a, b)` work as well as `x.to_text` and "
             "`a.compare(b)` for every type with `Text` or `Order`",
             "A case without fields can be the receiver directly, as in `Less.compare(Greater)` "
             "or `Folder.to_text`.")
        assert run(binary, ["run", str(receivers)], 0).stdout == b"Less Folder\n"
        for path, printed in zip(qualified, (b"Less\n", b"[1]\n")):
            assert run(binary, ["run", str(path)], 0).stdout == printed, path
        for command in ("check", "run", "test"):
            refuses([command, str(unfinished)], 1103,
                    "this constant's call dependencies are not implemented",
                    "`mo check` reports E1103 for a form the checker has not implemented, with a "
                    'headline that says so, such as "this constant\'s call dependencies are not '
                    'implemented".')
            refuses([command, str(deferred)], 1150,
                    "this constant's deferred function needs globals not yet initialized",
                    "It reports E1150 for a form it checks but cannot compile yet, such as a "
                    "constant's function value that reads a later constant. The headline is "
                    '"this constant\'s deferred function needs globals not yet initialized".')

        # E1400 for a promise whose function takes a function; the rest run with --only.
        twice = folder / "twice.mo"
        twice.write_text('module Twice\n\nfn twice(xs: List(Int), f: fn(Int) : Int) : List(Int)\n'
                         '  promise "keeps the length"\n    result.size == xs.size\n'
                         '    allows xs: [1], f: fn(x) x end, result: [1]\n'
                         '    forbids xs: [1], f: fn(x) x end, result: []\n  end\n\n'
                         '  xs.map(fn(x) f(f(x)) end)\nend\n\n'
                         'fn one() : Int\n  examples\n    one() == 1\n  end\n\n  1\nend\n',
                         encoding="utf-8")
        run(binary, ["check", str(twice)], 0)
        refuses(["test", str(twice)], 1400, "this build cannot generate inputs for this promise",
                "`mo test` exits 1 with E1400 when it cannot build the inputs of a selected "
                "promise, such as one whose function takes a function. The headline is "
                '"this build cannot generate inputs for this promise".')
        says("Run the other examples, promises and tests by name with `--only`, as `--list` "
             "prints them.")
        listed = run(binary, ["test", "--list", str(twice)], 0).stdout
        assert listed.decode().splitlines() == [
            "Twice.twice.promise.1", "Twice.one.example.1"], listed
        mutant_listed = run(binary, ["test", "--mutants", "--list", str(twice)], 0).stdout
        assert mutant_listed.decode().splitlines() == [
            "Twice.twice.promise.1", "Twice.one.example.1",
            f"mutant {twice}:18:3 Twice.one: int, `1` to `2`",
            "function Twice.twice: 0 mutants", "function Twice.one: 1 mutant"], mutant_listed
        result = run(binary, ["test", "--only", "Twice.one.example.1", str(twice)], 0)
        assert result.stdout.endswith(b"1 passed, 0 failed, 0 stopped\n")

    says("The `lib/` folder holds library modules written in Mo, embedded into the `mo` binary so "
         "a single `mo` executable runs anywhere without an external `lib/` directory "
         "(`--lib DIR` overrides the embedded library with a folder on disk).",
         "Every `mo` command loads them with your program, `Command`, `Crypto`, `Date`, `Http`, `Json` and `Unicode16` only "
         "when a `use` line names them.",
         "`mo check lib/list.mo` and `mo test lib/list.mo` check and test one of them on its own.")
    for library_file in sorted((root / "lib").rglob("*.mo")):
        run(binary, ["check", str(library_file)], 0)
        result = run(binary, ["test", str(library_file)], 0, timeout=300)
        assert result.stdout.endswith(b", 0 failed, 0 stopped\n"), (library_file, result.stdout)
    with tempfile.TemporaryDirectory() as folder:
        source = pathlib.Path(folder) / "main.mo"
        source.write_text("module Main\nuse Json\nfn main(console: Console)\n"
                          "  console.print(Json.Null.to_text)\nend\n"
                          "test \"own\"\n  assert 1 == 1\nend\n")
        core = run(binary, ["check", "--emit=core", str(source)], 0).stdout
        assert b'"Json.' in core and b"Main.test.1" in core, core[:200]
        for kind in (b".example.", b".promise.", b".allows.", b".forbids."):
            assert kind not in core, ("a program lowers a library module's " + kind.decode(), core[:200])
        assert core.count(b".test.") == core.count(b"Main.test.1"), core[:200]
    for name in ("map", "filter", "filter_map", "flat_map", "reduce", "find", "any?", "all?", "count",
                 "sort_by", "sort_by_desc", "sort", "min", "max", "group_by", "Map.update",
                 "Option.or_fail", "Option.map", "Result.map", "Result.map_error",
                 "FileError.to_text", "Json.parse", "pretty"):
        says(f"`{name}`")
    says("`lib/text.mo` has no functions, only tests of `Text`'s operations")
    says("`lib/set.mo` likewise tests `Set`'s operations, `List.to_set` and Set text forms")
    says("`lib/time.mo` has no functions, only tests of `Time`'s, `Duration`'s and "
         "`Clock.fixed`'s operations, run by `mo test lib/time.mo`.")
    says("`lib/unicode16.mo` is the `Unicode16` module.",
         "After `use Unicode16`, its built-in `graphemes`, `display_width`, `normalize_nfc`, "
         "`lowercase` and `uppercase` operations follow Unicode 16.0.",
         "`mo test lib/unicode16.mo` tests them.")
    says("`Unicode16.letter?(c: Char) : Bool` tests Unicode 16.0 General_Category L",
         "`Unicode16.digit?(c: Char) : Bool` tests `Nd` (decimal digits).",
         "Neither includes `Nl`, `No` or marks, normalizes, or uses locale tailoring.")
    says("After `use Json` (or `use Shop.Account`), a bare `Json` (or `Account`) in a type is the "
         "module's own type of that name, as reference 10.3 says: write `fn f(value: Json)`. "
         "`Json.Json` means the same type.")
    with tempfile.TemporaryDirectory() as folder:
        source = pathlib.Path(folder) / "main.mo"
        program = ("module Main\nuse {use}\nfn f(value: {type}) : Text\n  value.to_text\nend\n"
                   "fn main(console: Console)\n  console.print(f(Json.Null))\nend\n")
        for use, type_name in (("Json", "Json"), ("Json", "Json.Json"), ("Json{Json}", "Json")):
            source.write_text(program.format(use=use, type=type_name))
            result = run(binary, ["run", str(source)], 0)
            assert result.stdout == b"null\n", (use, result.stdout, result.stderr)
    says("with lookup by `get`, `at`, `path`, `text_at`, `int_at`, `bool_at` and `list_at`: "
         "`reply.text_at(\"items.0.user.login\")` is `Some` of the string there, and `None` when "
         "the path is absent or holds another kind of value")
    with tempfile.TemporaryDirectory() as folder:
        source = pathlib.Path(folder) / "main.mo"
        source.write_text(
            "module Main\nuse Json\nfn main(console: Console)\n"
            "  case Json.parse(\"{\\\"items\\\": [{\\\"user\\\": {\\\"login\\\": \\\"mo\\\"}}]}\")\n"
            "    Ok(reply):\n"
            "      console.print(\"#{reply.text_at(\"items.0.user.login\")}\")\n"
            "      console.print(\"#{reply.text_at(\"items.1.user.login\")}\")\n"
            "      console.print(\"#{reply.text_at(\"items.0.user\")}\")\n"
            "    Error(_): console.print(\"not JSON\")\n  end\nend\n")
        result = run(binary, ["run", str(source)], 0)
        assert result.stdout == b'Some("mo")\nNone\nNone\n', (result.stdout, result.stderr)
    says("`Network` and `Connection` run (reference section 14).",
         "`network.connect_secure(host, port, within)` opens a TLS 1.2+ connection verified "
         "against the system's root certificates and `host` (returning "
         "`Error(Network.Certificate(reason: reason))` on certificate or host mismatch)",
         "`network.allow_hosts(hosts)` narrows a real or fake `Network` to the `\"host\"` or "
         "`\"host:port\"` destinations in `hosts`, refusing any other destination with "
         "`Error(Network.Refused(host: host, port: port))`.",
         "`NetworkError`'s cases are written with `Network.` in front, as in "
         "`Error(Network.TimedOut)`.",
         "`Network.fake({\"example.org:80\": {request: reply}})` answers a `connect` or "
         "`connect_secure` connection that sends exactly `request` with `reply`. Any other "
         "request gets `Error(Network.Broken(reason: reason))`, and the reason names the line "
         "that differs.",
         "`bytes.slice(from, to)` and `Bytes.join(parts)` cut and join `Bytes`.")
    with tempfile.TemporaryDirectory() as folder:
        source = pathlib.Path(folder) / "main.mo"
        source.write_text(
            "module Main\n\ntest \"a fake network answers one request\"\n"
            "  request = \"GET /\\r\\nA: 1\\r\\n\".bytes\n"
            "  network = Network.fake({\"example.org:80\": {request: \"reply\".bytes}, "
            "\"example.org:443\": {request: \"secure\".bytes}})\n"
            "  narrow = network.allow_hosts([\"example.org:80\", \"example.org:443\"])\n"
            "  assert narrow.connect(\"other.org\", 80, 1.second) is\n"
            "    Error(Network.Refused(host: \"other.org\", port: 80))\n"
            "  assert narrow.connect(\"example.org\", 80, 1.second) is Ok(held)\n"
            "  assert held.send(request) == Ok(Unit)\n"
            "  assert held.receive(3) == Ok(\"rep\".bytes)\n"
            "  assert narrow.connect_secure(\"example.org\", 443, 1.second) is Ok(tls)\n"
            "  assert tls.send(request) == Ok(Unit)\n"
            "  assert tls.receive(6) == Ok(\"secure\".bytes)\n"
            "  assert network.connect(\"example.org\", 80, 1.second) is Ok(other)\n"
            "  assert other.send(\"GET /\\r\\nA: 2\\r\\n\".bytes) == Ok(Unit)\n"
            "  reason = \"no reply for \\\"GET /\\\": sent \\\"A: 2\\\\r\\\\n\\\", "
            "expected \\\"A: 1\\\\r\\\\n\\\"\"\n"
            "  assert other.receive(3) == Error(Network.Broken(reason: reason))\n"
            "  assert other.send(request) == Error(Network.Closed)\n"
            "  assert Bytes.join([\"ab\".bytes, \"cd\".bytes]).slice(1, 3) == \"bc\".bytes\n"
            "end\n")
        result = run(binary, ["test", str(source)], 0)
        assert b"1 passed, 0 failed, 0 stopped" in result.stdout, (result.stdout, result.stderr)
    says("`clock.sleep(duration)` pauses on a real `Clock` and advances `Clock.fixed` by "
         "`duration` without waiting.")
    says("`.form(pairs)`", "`.max_body(bytes)`", "`response.headers_named(name)`")
    says("`sqlite.open(\":memory:\")` opens a private ephemeral in-memory database without "
         "touching disk")
    with tempfile.TemporaryDirectory() as folder:
        source = pathlib.Path(folder) / "main.mo"
        source.write_text(
            "module Main\n\n"
            "fn main(sqlite: Sqlite, clock: Clock, io: Console) : Int\n"
            "  clock.sleep(1.millisecond)\n"
            "  case sqlite.open(\":memory:\")\n"
            "    Ok(db):\n"
            "      _c = db.run(\"CREATE TABLE t(n INT)\", [])\n"
            "      _i = db.run(\"INSERT INTO t VALUES (?)\", [Sqlite.Integer(9)])\n"
            "      case db.query(\"SELECT n FROM t\", [])\n"
            "        Ok(r): io.print(\"#{r.rows.size}\")\n"
            "        Error(_): return 1\n"
            "      end\n"
            "      0\n"
            "    Error(_): 1\n"
            "  end\n"
            "end\n")
        result = run(binary, ["run", "--state-dir", folder, str(source)], 0)
        assert result.stdout == b"1\n", (result.stdout, result.stderr)
        assert not (pathlib.Path(folder) / ":memory:").exists()
    says("`.send(network)` gives `Result(Response, Http.HttpError)`.",
         "A status such as 404 is a response and not an error.",
         "`Network.fake(Http.replies([(request, Http.reply(200, body))]))` answers exactly that "
         "request, so the test builds the request the program builds. The order of `header` calls "
         "with different names does not matter.")
    example = blocks[1].split("```\n", 1)[0]
    with tempfile.TemporaryDirectory() as folder:
        source = pathlib.Path(folder) / "issues.mo"
        source.write_text(
            example +
            "\ntest \"what the guide says about a status, https and the order of headers\"\n"
            "  one = Http.get(\"http://h/\").header(\"B\", \"2\").header(\"A\", \"1\")\n"
            "  other = Http.get(\"http://h/\").header(\"A\", \"1\").header(\"B\", \"2\")\n"
            "  secure = Http.get(\"https://h/\")\n"
            "  network = Network.fake(Http.replies([\n"
            "    (one, Http.reply(404, \"gone\")),\n"
            "    (secure, Http.reply(200, \"encrypted\"))\n"
            "  ]))\n"
            "  assert other.send(network).map(fn(r) (r.status, r.ok?, r.text) end) ==\n"
            "    Ok((404, false, Some(\"gone\")))\n"
            "  assert other.header(\"C\", \"3\").send(network) is\n"
            "    Error(Http.Unreachable(Network.Broken(reason: _)))\n"
            "  assert secure.send(network) == Ok(Http.reply(200, \"encrypted\"))\n"
            "end\n")
        result = run(binary, ["test", str(source)], 0)
        assert b"2 passed, 0 failed, 0 stopped" in result.stdout, (result.stdout, result.stderr)
    says("`Sqlite` and `Sqlite.Database` run with `PRAGMA foreign_keys = ON` (reference section 14).",
         "`sqlite.open(path)` opens or creates a SQLite database inside the capability's folder",
         "`database.run(statement, values)` runs one statement with `Sqlite.Value` arguments",
         "`database.execute(statement, values)` returns `Sqlite.ExecResult(changes, last_insert_rowid)`",
         "`database.query(statement, values)` returns `Sqlite.Rows(columns, rows)`.",
         "`Sqlite.decode[T](rows)`, `Sqlite.decode_one[T](rows)` and `Sqlite.decode_optional[T](rows)` decode rows by column name into a struct `T`",
         "`Sqlite.transaction(db, ctx, work, on_sql)` runs `BEGIN IMMEDIATE ... COMMIT` with automatic `ROLLBACK` on `Error`.",
         "`SqliteError` cases (`OutsideFolder`, `NotPermitted`, `SqlFailed`) and `Sqlite.Value` cases are written with `Sqlite.` in front.",
         "`Sqlite.fake()` keeps databases in memory so two opens of one path on one fake share tables and rows.")
    says("This build checks, formats, fixes, tests and serves declarative `api ... end` HTTP/REST "
         "servers (reference 11.6) alongside `lib/api.mo` (`use Api`).",
         "`static \"/prefix\" from \"rel/dir\"`",
         "nested `scope \"/prefix\" ... end` blocks (up to 8 levels deep)",
         "`mo serve FILE [--port N] [--bind HOST] [--workers N] [--watch] [--data DIR]`",
         "In `mo test`, `ApiTest.send(Module, req, ...)` dispatches an `Api.Request` directly in "
         "memory with zero sockets (including static mounts via `ReadFiles`, `Sqlite.in_memory()` "
         "or `Sqlite.fake()`, `Env.of(...)`, `Actor.start(ActorType)`, etc.).")
    says("This build also checks, formats, fixes, tests and runs composable in-process "
         "`actor Name ... end` capabilities (reference 11.7, E0111).",
         "`mo test` automatically runs `Module.service.recovery` and `Module.service.promise.N` "
         "on every `actor`.")
    with tempfile.TemporaryDirectory() as folder:
        source = pathlib.Path(folder) / "main.mo"
        source.write_text(
            "module Main\nuse Api\n\n"
            "struct Count\n  value: Int\nend\n\n"
            "actor Counter\n"
            "  state Count = Count(value: 0)\n"
            "  calls add, current\n"
            "  promise on add\n"
            "    after.value == before.value + n\n"
            "  end\n"
            "end\n\n"
            "fn add(s: Count, _t: Time, n: Int) : (Count, Int)\n"
            "  (Count(value: s.value + n), s.value + n)\n"
            "end\n\n"
            "fn current(s: Count, _t: Time) : Int\n"
            "  s.value\n"
            "end\n\n"
            "api\n"
            "  actor ctr: Counter\n"
            "  post \"/add/:n\" post_add\n"
            "end\n\n"
            "fn post_add(ctr: Counter, n: Int) : Int\n"
            "  ctr.add(n)\n"
            "end\n\n"
            "test \"actor claim\"\n"
            "  ctr = Actor.start(Counter)\n"
            "  assert ctr.add(4) == 4\n"
            "  assert ctr.current == 4\n"
            "  assert ApiTest.send(Main, Api.post(\"/add/3\"), ctr).text == \"7\"\n"
            "end\n")
        result = run(binary, ["test", str(source)], 0)
        assert b"3 passed, 0 failed, 0 stopped" in result.stdout, (result.stdout, result.stderr)
    with tempfile.TemporaryDirectory() as folder:
        source = pathlib.Path(folder) / "main.mo"
        source.write_text(
            "module Main\n\ntest \"fake sqlite shares tables and enforces sandbox rules\"\n"
            "  sqlite = Sqlite.fake()\n"
            "  assert sqlite.open(\"app.db\") is Ok(first)\n"
            "  assert first.run(\"CREATE TABLE items(n INT, s TEXT)\", []) == Ok(Unit)\n"
            "  assert first.run(\"INSERT INTO items VALUES (?, ?)\", [Sqlite.Integer(7), Sqlite.String(\"mo\")]) == Ok(Unit)\n"
            "  assert sqlite.open(\"./app.db\") is Ok(second)\n"
            "  assert second.query(\"SELECT n, s FROM items\", []) ==\n"
            "    Ok(Sqlite.Rows(columns: [\"n\", \"s\"], rows: [[Sqlite.Integer(7), Sqlite.String(\"mo\")]]))\n"
            "  assert first.run(\"ATTACH 'b.db' AS b\", []) is Error(Sqlite.NotPermitted(reason: _))\n"
            "  assert sqlite.open(\"../escape.db\") is Error(Sqlite.OutsideFolder(path: escaped))\n"
            "  assert escaped == \"../escape.db\"\n"
            "end\n")
        result = run(binary, ["test", str(source)], 0)
        assert b"1 passed, 0 failed, 0 stopped" in result.stdout, (result.stdout, result.stderr)
    says("After `use Command`, `Command` runs confined external programs without a shell (reference section 14).",
         "`command.within(folder)` narrows the working folder",
         "`command.allow_programs(programs)` monotonically intersects allowed program basenames",
         "`Command.fake(Command.replies(exchanges))` answers `(Command.Spec, Command.Output)` pairs")
    with tempfile.TemporaryDirectory() as folder:
        source = pathlib.Path(folder) / "main.mo"
        source.write_text(
            "module Main\nuse Command\n\ntest \"fake command claim\"\n"
            "  cmd = Command.fake(Command.replies([\n"
            "    (Command.spec(\"mo\", [\"test\"]).in_folder(\"sub\"), Command.ok_output(\"ok\\n\"))\n"
            "  ]))\n"
            "  scoped = cmd.within(\"sub\").allow_programs([\"mo\"])\n"
            "  assert scoped.run(\"mo\", [\"test\"], 5.seconds) ==\n"
            "    Ok(Command.Output(status: 0, stdout: \"ok\\n\", stderr: \"\"))\n"
            "  assert scoped.run(\"sh\", [], 5.seconds) == Error(Command.NotPermitted(program: \"sh\"))\n"
            "  assert cmd.within(\"../x\").run(\"mo\", [], 5.seconds) == Error(Command.OutsideFolder(path: \"../x\"))\n"
            "end\n")
        result = run(binary, ["test", str(source)], 0)
        assert b"1 passed, 0 failed, 0 stopped" in result.stdout, (result.stdout, result.stderr)
    says("`SecureRandom` draws cryptographically secure random bytes via `rng.bytes(count)`",
         "After `use Crypto`, `Crypto.sha256`, `Crypto.hmac_sha256`, `Crypto.constant_time_eq?`, "
         "`Crypto.seal`, `Crypto.open`, `Crypto.hash_password` and `Crypto.verify_password`",
         "After `use Date`, `Date` provides opaque validated Gregorian dates")
    with tempfile.TemporaryDirectory() as folder:
        source = pathlib.Path(folder) / "main.mo"
        source.write_text(
            "module Main\nuse Crypto\nuse Date\nuse Json\n\ntest \"crypto and date claim\"\n"
            "  rng = SecureRandom.seeded(7)\n"
            "  key = rng.bytes(32)\n"
            "  nonce = rng.bytes(12)\n"
            "  assert Crypto.seal(key, nonce, \"hello\".bytes, \"aad\".bytes) is Ok(ct)\n"
            "  assert Crypto.open(key, nonce, ct, \"aad\".bytes) == Ok(\"hello\".bytes)\n"
            "  assert Bytes.from_hex(key.to_hex) == Some(key)\n"
            "  assert Bytes.from_base64url(key.to_base64url) == Some(key)\n"
            "  assert Date.parse_iso(\"2026-01-31\") is Some(d)\n"
            "  assert d.add_months(1) == Date.from_ymd(2026, 2, 28)\n"
            "  assert Json.encode(d) == \"\\\"2026-01-31\\\"\"\n"
            "end\n")
        result = run(binary, ["test", str(source)], 0)
        assert b"1 passed, 0 failed, 0 stopped" in result.stdout, (result.stdout, result.stderr)
    says("Under `lib/arvo/` (loaded lazily on `use Arvo.*`), the pure-Mo Arvo web framework provides "
         "`Arvo.Cookie`, `Arvo.Session`",
         "`Arvo.Csrf`", "`Arvo.Flash`", "`Arvo.Form`", "`Arvo.Inertia`",
         "`Arvo.Changeset`, `Arvo.Migrations`, `Arvo.Audit`, `Arvo.Auth`, and `Arvo.WebTest`",
         "`Arvo.Jobs` (`use Arvo.Jobs`) provides the durable SQLite `arvo_jobs` queue",
         "`ApiTest.run_job(Module, name, payload, ...)`",
         "When a `mo.toml` manifest sits in the entry `.mo` file's folder or an ancestor folder")
    with tempfile.TemporaryDirectory() as folder:
        standalone = pathlib.Path(folder) / "mo"
        shutil.copy2(binary, standalone)
        dep_dir = pathlib.Path(folder) / "dep" / "src"
        dep_dir.mkdir(parents=True)
        (dep_dir / "helper.mo").write_text(
            "module Helper\nexpose tag\nfn tag() : Text\n  \"pkg-ok\"\nend\n")
        app_dir = pathlib.Path(folder) / "app"
        app_dir.mkdir()
        (app_dir / "mo.toml").write_text(
            "[package]\nname = \"guide-app\"\n[dependencies]\nhelper = { path = \"../dep\" }\n")
        source = app_dir / "main.mo"
        source.write_text(
            "module Main\n"
            "use Api\n"
            "use Json\n"
            "use Arvo.Cookie\n"
            "use Arvo.Jobs\n"
            "use Arvo.Session\n"
            "use Arvo.WebTest\n"
            "use Helper\n\n"
            "api\n"
            "  job \"ping\" run_ping every 5.minutes\n"
            "  get \"/ok\" get_ok\n"
            "end\n\n"
            "fn run_ping(_payload: Json) : Unit\n"
            "  Unit\n"
            "end\n\n"
            "fn get_ok() : Text\n"
            "  Helper.tag()\n"
            "end\n\n"
            "test \"standalone binary loads embedded Arvo.*, jobs, and mo.toml dependencies\"\n"
            "  res = Cookie.set(Api.text_response(200, Helper.tag()), \"sid\", \"abc\", Cookie.default_options())\n"
            "  client = WebTest.absorb(WebTest.new(), res)\n"
            "  assert WebTest.cookie(client, \"sid\") == Some(\"abc\")\n"
            "  assert Session.default_config(true).cookie_name == \"arvo_session\"\n"
            "  assert Jobs.backoff_seconds(2) == 60\n"
            "  assert ApiTest.run_job(Main, \"ping\", Json.Null) == Ok(Unit)\n"
            "end\n")
        result = subprocess.run([str(standalone), "test", str(source)],
                                cwd=folder, check=False, capture_output=True)
        assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)
        assert b"1 passed, 0 failed, 0 stopped" in result.stdout, (result.stdout, result.stderr)


def emit_command(binary):
    """mo emit FILE -o OUT writes the verified bytecode that mo run executes (MO-41)."""
    driver = binary.parent / "tests/test_cli.test"
    sources = {
        # Console output and an exit status, so both sides of the comparison have something to match.
        "console": 'module Main\nfn main(console: Console) : Int\nconsole.print("hello 日本語")\n37\nend\n',
        "silent": "module Main\nfn main()\nend\n",
    }
    with tempfile.TemporaryDirectory(prefix="mo-emit-") as directory:
        folder = pathlib.Path(directory)
        for name, text in sources.items():
            path = folder / name / "main.mo"
            path.parent.mkdir()
            path.write_text(text)
            out = folder / f"{name}.bc"
            for args in (["emit", str(path), "-o", str(out)], ["emit", "-o", str(out), str(path)]):
                out.unlink(missing_ok=True)
                result = run(binary, args, 0)
                assert not result.stdout and not result.stderr
                data = out.read_bytes()
                assert data.startswith(b"MOBC")
            # The loader verifies the file as it loads it, then the program prints and exits as
            # mo run does on the source.
            ran = run(binary, ["run", str(path)], 37 if name == "console" else 0)
            loaded = run(driver, ["--run-bytecode", str(out)], 37 if name == "console" else 0)
            assert loaded.stdout == ran.stdout and loaded.stderr == ran.stderr, (name, loaded, ran)
            direct = run(binary, ["run", str(out)], 37 if name == "console" else 0)
            assert direct.stdout == ran.stdout and direct.stderr == ran.stderr, (name, direct, ran)
            # Emission is deterministic.
            again = folder / f"{name}-again.bc"
            run(binary, ["emit", str(path), "-o", str(again)], 0)
            assert again.read_bytes() == data
        # Emitting replaces an existing file whole.
        out = folder / "console.bc"
        out.write_bytes(b"x" * (1 << 20))
        run(binary, ["emit", str(folder / "console" / "main.mo"), "-o", str(out)], 0)
        assert out.read_bytes().startswith(b"MOBC") and out.stat().st_size < 1 << 20
        # Checks come first: a program with errors writes nothing and reports them as mo check does.
        bad = folder / "bad" / "main.mo"
        bad.parent.mkdir()
        bad.write_text("module Main\nfn main() : Int\n\"text\"\nend\n")
        target = folder / "bad.bc"
        refused = run(binary, ["emit", str(bad), "-o", str(target)], 1)
        assert refused.stderr == run(binary, ["check", str(bad)], 1).stderr and not target.exists()
        # A library has no entry, as for mo run.
        library = root / "tests/cli/library/main.mo"
        target = folder / "library.bc"
        refused = run(binary, ["emit", str(library), "-o", str(target)], 1)
        assert b"E0079" in refused.stderr and not target.exists()
        # The source is never the output.
        source = folder / "console" / "main.mo"
        before = source.read_bytes()
        refused = run(binary, ["emit", str(source), "-o", str(source)], 1)
        assert b"E1404" in refused.stderr and source.read_bytes() == before
        # Nor is any module the program imports.
        imported = folder / "imported"
        imported.mkdir()
        (imported / "main.mo").write_text(
            "module Main\nuse Left\n\nfn main(console: Console)\nconsole.print(\"#{Left.value()}\")\nend\n")
        left = imported / "left.mo"
        left.write_text("module Left\nexpose value\n\nfn value() : Int\n11\nend\n")
        before = left.read_bytes()
        refused = run(binary, ["emit", str(imported / "main.mo"), "-o", str(left)], 1)
        assert b"E1404" in refused.stderr and left.read_bytes() == before
        # With anchors, emit stops after checking them: it runs neither main nor a failing
        # anchor's program through to a file.
        anchored = folder / "anchored"
        anchored.mkdir()
        anchor_source = ('module Main\nfn twice(n: Int) : Int\npromise "returns twice its input"\n'
                         'result == n + n\nallows result: 6, n: 3\nforbids n: 4, result: 7\nend\n'
                         'n + n\nend\nfn main(console: Console) : Int\nconsole.print("main")\n37\nend\n')
        (anchored / "main.mo").write_text(anchor_source)
        target = folder / "anchored.bc"
        result = run(binary, ["emit", str(anchored / "main.mo"), "-o", str(target)], 0)
        assert not result.stdout and not result.stderr and target.read_bytes().startswith(b"MOBC")
        target.unlink()
        (anchored / "main.mo").write_text(anchor_source.replace("result: 6", "result: 5"))
        refused = run(binary, ["emit", str(anchored / "main.mo"), "-o", str(target)], 1)
        assert b"E1405" in refused.stderr and not refused.stdout and not target.exists()
        # A file that can't be written is E1404 with the OS's reason, and nothing is left behind.
        refused = run(binary, ["emit", str(source), "-o", str(folder / "missing" / "x.bc")], 1)
        assert b"E1404" in refused.stderr and b"No such file or directory" in refused.stderr
        full = pathlib.Path("/dev/full")
        if full.exists():
            refused = run(binary, ["emit", str(source), "-o", str(full)], 1)
            assert b"E1404" in refused.stderr
        # mo run executes emitted bytecode with CLI arguments, refuses service bytecode (E1408)
        # and corrupt bytecode (E1404), and a standalone copied mo binary runs with embedded lib/.
        arg_src = folder / "arg_prog" / "main.mo"
        arg_src.parent.mkdir()
        arg_src.write_text(
            'module Main\nuse Json\nfn main(args: Args, console: Console)\n'
            '  console.print("#{args.list.first or "none"} #{Json.Null.to_text}")\nend\n')
        arg_bc = folder / "arg_prog" / "out_bin"
        run(binary, ["emit", str(arg_src), "-o", str(arg_bc)], 0)
        arg_src.unlink()
        assert run(binary, ["run", str(arg_bc), "Ada", "Grace"], 0).stdout == b"Ada null\n"
        corrupt = folder / "corrupt.bc"
        corrupt.write_bytes(b"MOBCbroken")
        assert b"E1404" in run(binary, ["run", str(corrupt)], 1).stderr
        svc_bc = folder / "svc.bc"
        run(binary, ["emit", "tests/errors/e1408/main.mo", "-o", str(svc_bc)], 0)
        assert b"E1408" in run(binary, ["run", str(svc_bc)], 1).stderr
        portable_dir = folder / "portable"
        portable_dir.mkdir()
        portable_mo = portable_dir / "mo"
        shutil.copy2(binary, portable_mo)
        (portable_dir / "app.mo").write_text(
            'module App\nuse Http\nuse Json\nfn main(console: Console)\n'
            '  method = Http.get("http://example.com").method\n'
            '  console.print("#{[1, 2, 3].map(fn(x) x * 2 end)} #{Json.int(7).to_text} #{method}")\n'
            'end\n')
        standalone = subprocess.run([str(portable_mo), "run", "app.mo"], cwd=portable_dir,
                                    capture_output=True, timeout=30)
        assert standalone.returncode == 0 and standalone.stdout == b"[2, 4, 6] 7 GET\n", standalone
        assert b"E1102" in run(binary, ["check", "--lib", str(folder / "no-such-lib"), str(source)], 1).stderr
    def mistake(args, message):
        r = run(binary, args, 2)
        assert not r.stdout and r.stderr == message.encode() + b"\n", (args, r.stderr)

    good = "tests/cli/good/main.mo"
    mistake(["emit", good], 'emit needs -o FILE: "emit"')
    mistake(["emit", "-o", "x.bc"], 'command needs a file: "emit"')
    mistake(["emit", good, "-o"], 'flag needs a nonempty value: "-o"')
    mistake(["emit", good, "-o", ""], 'flag needs a nonempty value: "-o" ""')
    mistake(["emit", good, "-o", "-x.bc"], 'flag needs a nonempty value: "-o" "-x.bc"')
    mistake(["emit", good, "-o", "a.bc", "-o", "b.bc"], 'flag may be given only once (except --only): "-o"')
    mistake(["emit", good, good, "-o", "a.bc"], f'command takes exactly one file: "{good}"')
    for flag in ("--json", "--emit=core", "--max-heap", "--list"):
        args = ["emit", flag, *(["1GiB"] if flag == "--max-heap" else []), good, "-o", "a.bc"]
        mistake(args, f'flag is not accepted by this command: "{flag}"')
    for command in ("check", "run", "test", "format", "fix"):
        mistake([command, "-o", "a.bc", good], f'flag is not accepted by this command: "-o"')
    for args in (["--list"], ["--json"], ["--calls", "5"], ["--seconds", "5"], ["--strength", "x"]):
        mistake(["test", "--bundle", *args, good],
                "test --bundle runs upgrade checks over the whole bundle; "
                'it takes no --list, --json, --only, --in, --calls, --seconds or --strength: "--bundle"')
    assert not (root / "a.bc").exists()


def install_command(binary):
    """mo install compiles a CLI program to bytecode + launcher and honors root/state-dir grants."""
    with tempfile.TemporaryDirectory(prefix="mo-install-") as directory:
        folder = pathlib.Path(directory)
        bin_dir = folder / "bin"
        other_cwd = folder / "elsewhere"
        target_root = folder / "target-project"
        state_dir = folder / "tool-state"
        other_cwd.mkdir()
        target_root.mkdir()
        state_dir.mkdir()
        (target_root / "sample.mo").write_text("module Sample\n\nfn add(a: Int, b: Int) : Int\n  a + b\nend\n")

        # 1. Install tools/moloc/main.mo -> derives tool name "moloc", runs from another CWD
        #    with --mo-root pointing at target_root.
        installed = run(binary, ["install", "tools/moloc/main.mo", "--bin-dir", str(bin_dir)], 0)
        launcher = bin_dir / "moloc"
        bc_file = bin_dir / ".mo-libexec" / "moloc.bc"
        assert launcher.exists() and bc_file.read_bytes().startswith(b"MOBC")
        assert installed.stdout.decode().strip() == f"installed {launcher.resolve()}"
        ran = subprocess.run([str(launcher), "--mo-root", str(target_root), "."],
                             cwd=other_cwd, capture_output=True, timeout=30)
        assert ran.returncode == 0 and b"Mo " in ran.stdout and not ran.stderr, ran

        # 2. Install a tool using both Files and Sqlite with a baked-in --state-dir; verify CWD
        #    files are read while the SQLite database stays inside state_dir, not other_cwd.
        spike_dir = folder / "spike-tool"
        spike_dir.mkdir()
        (spike_dir / "main.mo").write_text(
            'module Main\n'
            'fn main(args: Args, files: ReadFiles, sqlite: Sqlite, console: Console) : Int\n'
            '  name = args.list.first or "input.txt"\n'
            '  body = case files.read_text(name)\n'
            '    Ok(t): t\n'
            '    Error(_): "missing"\n'
            '  end\n'
            '  case sqlite.open("history.sqlite")\n'
            '    Error(_): 2\n'
            '    Ok(db):\n'
            '      _r1 = db.run("CREATE TABLE IF NOT EXISTS log (body TEXT)", [])\n'
            '      _r2 = db.run("INSERT INTO log VALUES (?)", [Sqlite.String(body)])\n'
            '      case db.query("SELECT COUNT(*) FROM log", [])\n'
            '        Ok(rows):\n'
            '          first = rows.rows.first or []\n'
            '          n = if first.first is Some(Sqlite.Integer(v)): v else: -1\n'
            '          console.print("#{body.trim}:#{n}")\n'
            '          0\n'
            '        Error(_): 3\n'
            '      end\n'
            '  end\n'
            'end\n')
        (other_cwd / "input.txt").write_text("alpha\n")
        (target_root / "input.txt").write_text("beta\n")
        # Install from inside spike_dir using relative "main.mo" -> derives parent name "spike-tool"
        rel_inst = subprocess.run(
            [str(binary), "install", "main.mo", "--bin-dir", str(bin_dir),
             "--state-dir", str(state_dir)],
            cwd=spike_dir, capture_output=True, timeout=30)
        assert rel_inst.returncode == 0 and not rel_inst.stderr, rel_inst
        spike_bin = bin_dir / "spike-tool"
        assert spike_bin.exists()
        r1 = subprocess.run([str(spike_bin)], cwd=other_cwd, capture_output=True, timeout=30)
        assert r1.returncode == 0 and r1.stdout == b"alpha:1\n", r1
        assert not (other_cwd / "history.sqlite").exists()
        assert (state_dir / "history.sqlite").exists()
        r2 = subprocess.run([str(spike_bin), "--mo-root", str(target_root)],
                            cwd=other_cwd, capture_output=True, timeout=30)
        assert r2.returncode == 0 and r2.stdout == b"beta:2\n", r2
        # Missing --mo-root value exits 2 cleanly
        bad_override = subprocess.run([str(spike_bin), "--mo-root"],
                                      cwd=other_cwd, capture_output=True, timeout=30)
        assert bad_override.returncode == 2 and b"--root" in bad_override.stderr

        # 3. Default ~/.local/bin via HOME, --name override, and refusal controls
        fake_home = folder / "fake-home"
        fake_home.mkdir()
        env = dict(subprocess.os.environ, HOME=str(fake_home))
        run(binary, ["install", "examples/greet.mo", "--name", "greeter"], 0, env=env)
        greeter = fake_home / ".local" / "bin" / "greeter"
        assert greeter.exists()
        g_out = subprocess.run([str(greeter), "Robert"], cwd=other_cwd, env=env,
                               capture_output=True, timeout=30)
        assert g_out.returncode == 0 and g_out.stdout == b"Hello, Robert!\n", g_out

        # Refusals: library without main (E0079), service (E1408), missing --root (E1404),
        # symlink at target launcher (E1404), invalid --name (exit 2).
        assert b"E0079" in run(binary, ["install", "tests/cli/library/main.mo",
                                        "--bin-dir", str(bin_dir)], 1).stderr
        assert b"E1408" in run(binary, ["install", "tests/errors/e1408/main.mo",
                                        "--bin-dir", str(bin_dir)], 1).stderr
        assert b"E1404" in run(binary, ["install", "examples/greet.mo", "--bin-dir", str(bin_dir),
                                        "--root", str(folder / "missing-dir")], 1).stderr
        sym = bin_dir / "symtool"
        sym.symlink_to(other_cwd / "input.txt")
        assert b"E1404" in run(binary, ["install", "examples/greet.mo", "--bin-dir", str(bin_dir),
                                        "--name", "symtool"], 1).stderr
        assert run(binary, ["install", "examples/greet.mo", "--name", "../bad"], 2).returncode == 2
