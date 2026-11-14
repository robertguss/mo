import pathlib
import subprocess
import tempfile

from cli_common import (
    SERVE_COUNTER, SERVE_OPERATOR_NAMES, SERVE_REVIEW_NAMES, SERVE_UPGRADE_NAMES, root, run,
)

flags = {
    "--json": ([], {"check", "test", "replay", "query", "history"}),
    "--list": ([], {"test"}),
    "--reference": ([], {"guide"}),
    "--only": (["Main.test.1"], {"test"}),
    "--in": (["Main"], {"test"}),
    "--max-stack": (["1MiB"], {"run", "test", "query"}),
    "--max-depth": (["100"], {"run", "test", "query"}),
    "--max-heap": (["1GiB"], {"run", "test", "query"}),
    "--calls": (["30"], {"test"}),
    "--seconds": (["3"], {"test"}),
    "--port": (["0"], {"serve"}),
    "--data": (["state"], {"serve", "replay", "query"}),
    "--allow-host": (["queue.example"], {"serve"}),
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
    # The live operator driver exercises these guide claims through the real endpoint.
    for name in SERVE_OPERATOR_NAMES:
        assert f"`{name}`" in guide
    # ci/check_offline.py exercises the offline commands' claims against real data folders.
    assert ("On the service host, `mo history`, `mo replay` and `mo query` read a stopped data "
            "folder or an artifact without starting the service, as Diagnose a failure offline "
            "says.") in guide
    assert "mo fetch URL --export failure ID --out failure.moex --token-env NAME" in guide
    # ci/check_upgrade_live.py and ci/check_review.py exercise these claims through mo serve,
    # mo push and mo review.
    for name in SERVE_UPGRADE_NAMES + SERVE_REVIEW_NAMES:
        assert f"`{name}`" in guide
    for sentence in ("Exit 1 means a refusal or client failure; exit 2 means invalid command usage, URL or base digest.",
                     "Only the nine operator views and `mo-intent-items` are read-only;",
                     "In this build approvals and the setting are recorded but do not yet gate an upgrade",
                     "any other URL is refused before either credential is read.",
                     "Pushing the active program again reports `already active` and changes "
                     "nothing",
                     "neither credential appears in output, events or the data folder.",
                     "a new GET replaces it."):
        assert sentence in guide, f"docs/agent-guide.md no longer says: {sentence}"
    assert ("A wrong version that stops is kept out of the rate, and so is one that does not check "
            "or changes a digest, unless a `counted` file in `DIR` lists it: a listed one stays in "
            "the rate as not killed.") in guide
    assert ("`mo intent FILE --against OLD` shows how the intent changed since the program `OLD`: "
            "items edited, with both texts, removed, added and moved to another function, and for "
            "an edited or moved item each declaration it reads that changed, before and after.") in guide
    blocks = guide.split("```mo\n")
    assert len(blocks) == 2, "the guide has exactly one Mo program"
    program = blocks[1].split("```\n", 1)[0]

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
    says("This build runs `mo check`, `mo run`, `mo test`, `mo format`, `mo fix`, `mo guide`, `mo emit`, `mo serve`, `mo fetch`, `mo push`, `mo review`, `mo replay`, `mo query`, `mo history` and `mo intent`.",
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
         "`passed Counter.service.recovery  counter.mo:3:1`, then `1 passed, 0 failed, 0 stopped`.",
         "When the start value stops, the check stops with that bug.",
         "`mo test FILE` checks its examples, promises, tests and recovery checks.",
         "1 when the program has errors or an example, promise, test or recovery check fails")
    with tempfile.TemporaryDirectory(prefix="mo-guide-recovery-") as directory:
        (pathlib.Path(directory) / "counter.mo").write_text(SERVE_COUNTER)
        counter = subprocess.run([str(binary), "test", "counter.mo"], cwd=directory,
                                 capture_output=True, timeout=30)
        assert counter.returncode == 0 and not counter.stderr, counter
        assert counter.stdout == (b"passed Counter.service.recovery  counter.mo:3:1\n"
                                  b"1 passed, 0 failed, 0 stopped\n"), counter.stdout
        listed = subprocess.run([str(binary), "test", "--list", "counter.mo"], cwd=directory,
                                capture_output=True, timeout=30)
        assert listed.stdout == b"Counter.service.recovery\n", listed
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
             "`mo format` and `mo fix` rewrite the files they are given.")
        says("`mo emit FILE -o OUT` checks the program as `mo check` does, then writes its bytecode "
             "to `OUT`. `-o` is required, and `OUT` is never a source file of the program. A module "
             "with neither `main` nor a service is E0079, as for `mo run`, and a file that cannot be written is "
             "E1404. This build has no command that runs a bytecode file; run the source with "
             "`mo run`.")
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
        assert listed == b"Twice.twice.promise.1\nTwice.one.example.1\n", listed
        result = run(binary, ["test", "--only", "Twice.one.example.1", str(twice)], 0)
        assert result.stdout.endswith(b"1 passed, 0 failed, 0 stopped\n")

    says("The `lib/` folder holds library modules written in Mo, and every `mo` command loads "
         "them with your program.",
         "`mo check lib/list.mo` and `mo test lib/list.mo` check and test one of them on its own.")
    for library_file in sorted((root / "lib").glob("*.mo")):
        run(binary, ["check", str(library_file)], 0)
        result = run(binary, ["test", str(library_file)], 0)
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
    for command in ("check", "run", "format", "fix"):
        mistake([command, "-o", "a.bc", good], f'flag is not accepted by this command: "-o"')
    for args in (["-o", "a.bc"], ["--bundle"], ["--bundle", "--list"], ["--list", "-o", "a.bc"]):
        mistake(["test", *args, good], 'test writes an item list only as --bundle --list -o FILE: "test"')
    assert not (root / "a.bc").exists()
