# Write a finished Mo program

This page is the procedure for a finished Mo program. Correct Mo is Language Reference v0.1. `mo guide --reference` prints that reference. Section 13.4 says this guide covers how to work with the `mo` command, a small complete program with its tests, and when a task is done.

## Know this build

The language headings below use the present tense of the reference.

This build runs `mo check`, `mo run`, `mo test`, `mo format`, `mo fix`, `mo guide`, `mo emit`, `mo serve`, `mo fetch`, `mo push`, `mo replay`, `mo query`, `mo history` and `mo diff`. The program below checks with no errors, runs, and passes its examples and its test.

`mo fix FILE` makes each edit that `mo check` shows as an error's only fix, checks again, and prints the errors that are left, as `mo check` does. It rewrites only the files it is given.

`mo emit FILE -o OUT` checks the program as `mo check` does, then writes its bytecode to `OUT`. `-o` is required, and `OUT` is never a source file of the program. A module with neither `main` nor a service is E0079, as for `mo run`, and a file that cannot be written is E1404. `mo run OUT [ARGS...]` loads, verifies and runs an emitted bytecode file directly without source files; an unreadable or invalid bytecode file is E1404, a service entry module is E1408, and a module without `main` is E0079.

This build checks, formats, fixes and serves `service ... end` blocks (reference 11.5). `mo test` never reads a service's token, and starts a service on its own only in the service's recovery check, below. Tests and examples can explicitly start one with `Simulation.start(Module, at)`, send registered tools with `Simulation.send(handle, Module.tool, args...)`, advance its fake clock, and crash/restart through the real log and snapshot path backed by an in-memory store. `send` returns the complete declared handler result inside another `Result`; an application `Error` is still inside `Ok`. Helpers may receive the handle. A promise whose inputs include a `Simulation` is E1400. `mo run` on a service entry module still refuses with E1408, "mo run does not start services yet". Importing a service module does not start it.

This build checks, formats, fixes, tests and serves declarative `api ... end` HTTP/REST servers (reference 11.6) alongside `lib/api.mo` (`use Api`). An `api` block declares `port`, `cors`, `sqlite name from "path"`, `actor name: ActorType`, `auth Principal via fn`, `get`/`post`/`put`/`patch`/`delete "/path/:id" handler`, and exhaustive `status Case = code` / `status Case(_) = code` clauses (E0108, E0109, E0110), automatically serving OpenAPI 3.1.0 at `GET /openapi.json` and interactive docs at `GET /docs`. `mo serve FILE [--port N] [--data DIR]` runs an `api` server over a concurrent worker pool on `127.0.0.1`, isolating each request so a bug (`stop` or limit) logs a failure and returns HTTP `500` without taking down the server. In `mo test`, `ApiTest.send(Module, req, ...)` dispatches an `Api.Request` directly in memory with zero sockets (using `Sqlite.in_memory()` or `Sqlite.fake()`, `Env.of(...)`, `Actor.start(ActorType)`, etc.).

This build also checks, formats, fixes, tests and runs composable in-process `actor Name ... end` capabilities (reference 11.7, E0111). An `actor` declares private `state`, `calls` (handlers taking `(state: State, now: Time, ...)` and returning `Reply`, `(State, Reply)` or `Result((State, Reply), E)`), optional `every` timers, and optional `promise` / `promise on call` invariants, with no `token` clause. Callers hold an opaque `Name` capability handle—declared with `actor limiter: RateLimiter` in an `api` block, listed on `main`, or started deterministically in tests with `Actor.start(RateLimiter)` / `Actor.start_at(RateLimiter, time)`—and invoke calls with dot syntax (`limiter.allow(key)`, `limiter.count`) or `Actor.send(limiter, RateLimiter.allow, key)` (plus `Actor.advance`, `Actor.crash` and `Actor.restart` in tests). `mo test` automatically runs `Module.service.recovery` and `Module.service.promise.N` on every `actor`.

`mo test` runs one recovery check for each service in the program, named after its module, such as `Counter.service.recovery` (reference 12.5). Its author writes nothing for it. It calls tools with generated arguments, advances the fake clock, crashes and restarts the service on an in-memory store, and checks that each restart recovers the state the service held before its crash. `--list` prints it after its module's other checks, and `--only` and `--in` select it. A service's own promises (reference 12.2) are checked on the same sequences. Each is named after its position, such as `Counter.service.promise.1`, listed just before the recovery check, and judged after every step it covers, restarts included. A failing promise prints its shrunk sequence with `broke` on the step that made it false, the states around that step and the condition that was false. Every service check runs its whole plan of 200 sequences, which `--calls` caps; `--seconds` does not apply. A promise body that runs past `MO_LIMIT_SERVICE_WORK` stops with that bug. For `counter.mo` under Run a service, `mo test counter.mo` prints `passed Counter.service.promise.1  counter.mo:8:3`, `passed Counter.service.promise.2  counter.mo:13:3` and `passed Counter.service.recovery  counter.mo:3:1`, then `3 passed, 0 failed, 0 stopped`. A failing check prints the numbered steps of a shrunk sequence, with literal arguments and each step's outcome, then the state before the crash and the state recovery produced. When the start value stops, the check stops with that bug.

`mo check --emit=bytecode FILE` prints the program's bytecode as indexed text, with a `services` section holding one descriptor for each module that declares a service, including imported modules. It works on a module without `main`. `mo emit FILE -o OUT` writes the same descriptors and accepts a service without `main`. Neither command evaluates a service's start value or reads its token, and `mo run` still does not start a service (E1408). `mo check --emit=core FILE` shows the start value and timer intervals as ordinary functions but no service registration, so Core output alone cannot reconstruct a service.

A service tool that returns exactly `State` or `Result(State, E)` gets E0101, "a service tool returns only its state". Only `(State, Reply)` and `Result((State, Reply), E)` save new state. `Option(State)`, `List(State)` and records containing state remain read-only replies. A `Result(State, E)` tool receives no fixes. For a bare `State` result, choose between returning `(state, Unit)` to save new state and returning the state inside a record as read-only data. `mo fix` applies neither alternative. The suggested adapters keep the tool's name and `##` comment, move the original function to a helper and rebind the module's references to that helper. Functions on the module's `expose` line receive no fixes because callers in other modules cannot be updated. Functions whose names end in `?` receive no fixes because a `?` name must still return `Bool`. A selected generic handler gets E0100, "a service handler is generic". State, handler inputs and handler results must contain neither capabilities nor function values; E0104 reports this restriction.

Service tool names cannot end in `?` (E0106). Tool arguments after State and Time, projected replies and application errors must have visible fields and cases, including nested types (E0105). Import a wire type with `(..)` to expose its representation. Private service state is not a wire type. E0101 takes precedence over these two checks.

`mo serve` connects a service to MCP clients over authenticated plain HTTP/1.1. HTTP policy, JSON-RPC validation and wire values run in embedded Mo; native code owns sockets, credentials and service dispatch. The transport's byte, node, work and memory ceilings are internal bounds, not language configuration. `mo run` still refuses a service entry module, and Simulation still provides no networking.

Public HTTP (`--port N`) exposes only the tools declared in `service ... end`. Passing `--admin-socket PATH` binds a mode-0600 Unix domain socket authenticated by OS filesystem permissions (unlinked on clean shutdown; a leftover socket from a crash is replaced, while a regular file or symlink at `PATH` is refused with E1409). Over `--admin-socket`, MCP discovery lists `mo-overview`, `mo-state`, `mo-mailbox`, `mo-timers`, `mo-memory`, `mo-supervisor`, `mo-log`, `mo-export` and `mo-failures`, followed by the upgrade tools `mo-upgrade-open`, `mo-upgrade-page` and `mo-upgrade-commit`, as Upgrade a running service says. Only the nine operator views are read-only; the three upgrade tools carry `readOnlyHint: false`, and commit carries `destructiveHint: true`. These read-only operator views share a separate admin rate bucket and do not invoke handlers or enter the mailbox, recovery log or supervisor counts. `mo-state` accepts an optional `path` array of field names and indexes; bounded output marks truncation and `complete: false`, and a value too large to show becomes `{"truncated_bytes": N}` while the values after it still render. A list view that does not fit one reply reports `truncated`, the number of items left out, and `next`, the arguments for the following page: `mo-mailbox` and `mo-timers` take `offset`, `mo-supervisor` takes `after` and `before`, and `mo-log` takes `after`. Separate observations are not one atomic snapshot. `mo-log` retains committed message metadata only since the last snapshot; in-memory supervisor events disappear when the process exits. `mo-export` with `{"kind":"state"}` starts an immutable MOEX v1 state export; use the returned `next` arguments to read subsequent base64 pages. Each page identifies its export, durable recovery sequence and encoded-state sequence (which can differ after a snapshot failure). The final page releases the pin. A new export replaces the previous state export; 60 seconds without a page expires it. Refused generations never return another export’s bytes. Pins use native memory outside the handler heap. `mo-failures` lists retained handler failures and current capture refusals; `after` selects the next record page. A handler failure reports `record: ID` or an explicit `record unavailable` reason before its reply. Application `Error` replies and admission refusals are excluded. Captures preserve pre-call state, arguments, arrival time, budgets, identity and bounded stack context; read-only failures never enter the recovery log. Retention keeps first/latest per kind, up to 64 records and 64 MiB of framed records. A repeating kind replaces its own latest record and never evicts another kind, so a repeat that does not fit is refused. A new kind evicts, as a whole, the kind whose latest record is oldest. Records survive snapshots. `mo-export` accepts `{"kind":"failure","failure":ID}` and has an independent failure pin. Run `mo fetch --admin-socket PATH --export failure ID --out failure.moex` for a verified private artifact, or `mo query FILE FUNCTION --admin-socket PATH` (with `--failure ID` for a failure) to query live state or a failure directly. On the service host, `mo history`, `mo replay` and `mo query` read a stopped data folder or an artifact without starting the service, as Diagnose a failure offline says.

A few forms the reference requires are still unfinished. `mo check` reports E1103 for a form the checker has not implemented, with a headline that says so, such as "this constant's call dependencies are not implemented". It reports E1150 for a form it checks but cannot compile yet, such as a constant's function value that reads a later constant. The headline is "this constant's deferred function needs globals not yet initialized". E1103 and E1150 mean this toolchain has not implemented that form. `mo run` and `mo test` report the same code for the same form.

If the reference offers another way to write the same behaviour from forms that compile, use that instead. Keep the unfinished form only when the reference requires it and offers no other way to write the same behaviour.

Every `Int`, `Float` and `Bytes` function listed in reference section 14 compiles and runs, whether called with a dot, through its type's module, or as a function value. This includes checked, wrapping and saturating integer arithmetic, integer conversions, float rounding and classification, and UTF-8 decoding.

`Byte.to_int` and `List.sum` also compile and run through dot calls, module calls and function values. `sum` supports `Int`, `BigInt`, `Decimal` and `Float`, adds from the left, and returns the type's zero for an empty list. Integer and decimal overflow stop the program, as with `+`.

Every `BigInt` function in reference section 14 compiles and runs, including `abs`, `min`, `max`, `pow`, `floor_div`, `floor_mod` and `to_int`. `Random.int` and the fake constructor `Random.seeded` also compile, including as function values.

`T.to_text(x)` and `T.compare(a, b)` work as well as `x.to_text` and `a.compare(b)` for every type with `Text` or `Order`, written through the type (`Text.compare`, `Option.to_text`), a type parameter, or the module that declares the type (`Shapes.to_text`), and `T.to_text` and `T.compare` are function values. A case without fields can be the receiver directly, as in `Less.compare(Greater)` or `Folder.to_text`. `Ordering`, `Entry`, and `Map` support `to_text` and interpolation. Maps print entries in insertion order, with text and character keys and values quoted and escaped.

`Env.get` runs on real or fake capabilities. Tests, examples and promises can construct a fake with `Env.of`.

`Network` and `Connection` run (reference section 14). `network.connect(host, port, within)` opens a TCP connection, `network.connect_secure(host, port, within)` opens a TLS 1.2+ connection verified against the system's root certificates and `host` (returning `Error(Network.Certificate(reason: reason))` on certificate or host mismatch), `connection.send(bytes)` and `connection.receive(most)` use it, and `within` is one deadline for all of them. `network.allow_hosts(hosts)` narrows a real or fake `Network` to the `"host"` or `"host:port"` destinations in `hosts`, refusing any other destination with `Error(Network.Refused(host: host, port: port))`. `NetworkError`'s cases are written with `Network.` in front, as in `Error(Network.TimedOut)`. In tests, examples and promises, `Network.fake({"example.org:80": {request: reply}})` answers a `connect` or `connect_secure` connection that sends exactly `request` with `reply`. Any other request gets `Error(Network.Broken(reason: reason))`, and the reason names the line that differs. `bytes.slice(from, to)` and `Bytes.join(parts)` cut and join `Bytes`.

`Sqlite` and `Sqlite.Database` run (reference section 14). `sqlite.open(path)` opens or creates a SQLite database inside the capability's folder, `database.run(statement, values)` runs one statement with `Sqlite.Value` arguments (`Sqlite.Null`, `Sqlite.Integer`, `Sqlite.Real`, `Sqlite.String`, `Sqlite.Blob`), and `database.query(statement, values)` returns `Sqlite.Rows(columns, rows)`. `SqliteError` cases (`OutsideFolder`, `NotPermitted`, `SqlFailed`) and `Sqlite.Value` cases are written with `Sqlite.` in front. `ATTACH` and `VACUUM INTO` fail with `Error(Sqlite.NotPermitted(reason: reason))`, and paths leaving the folder fail with `Error(Sqlite.OutsideFolder(path: path))`. In tests, examples and promises, `Sqlite.fake()` keeps databases in memory so two opens of one path on one fake share tables and rows.

After `use Http`, a program calls an API over a `Network` (reference section 14). `Http.get(url)`, `Http.post(url)` and `Http.request(method, url)` start a request (`http://` or `https://`), `.header(name, value)`, `.query(name, value)`, `.json(value)`, `.text(text)` and `.within(timeout)` change it, and `.send(network)` gives `Result(Response, Http.HttpError)`. A response has `status`, `headers` and `body`, with `response.ok?`, `response.header(name)`, `response.text` and `response.json`. A status such as 404 is a response and not an error. `send` follows no redirect and retries nothing. In a test, `Network.fake(Http.replies([(request, Http.reply(200, body))]))` answers exactly that request, so the test builds the request the program builds. The order of `header` calls with different names does not matter.

```mo
module Issues
use Http

fn title(network: Network, id: Int) : Result(Text, Http.HttpError)
  asked = Http.get("http://api.example.com/issues/#{id}").header("Accept", "application/json")
  response = try asked.send(network)
  return Error(Http.BadResponse(reason: "status #{response.status}")) if not response.ok?
  document = try response.json
  Ok(document.text_at("title") or "untitled")
end

test "a fake network answers an API call"
  asked = Http.get("http://api.example.com/issues/7").header("Accept", "application/json")
  network = Network.fake(Http.replies([(asked, Http.reply(200, "{\"title\": \"Crash\"}"))]))
  assert title(network, 7) == Ok("Crash")
end
```

`mo test` exits 1 with E1400 when it cannot build the inputs of a selected promise, such as one whose function takes a function. The headline is "this build cannot generate inputs for this promise". Keep the promise. Run the other examples, promises and tests by name with `--only`, as `--list` prints them.

For a false promise, `mo test` shrinks hidden inputs by deleting builder calls and simplifying their arguments, never by editing hidden fields. Same-call companions stay whole. Builder and structural shrinking share 200 additional subject trials and the original deadline; the result is not guaranteed minimal. Each generated tuple can record at most 4096 builder recipes, including recipes made during unsuccessful starts. If recording runs out, generation continues unchanged. A passing tuple still passes; a failing tuple keeps its original counterexample and reports E1404 with the headline "promise shrinking is limited because builder recording is incomplete".

For a promise on a generic function, `mo test` generates and shrinks inputs for every concrete instantiation already in the program, including anchors and program uses. It keeps complete type-argument lists, without cross-products or default types. `--list` and `--only` use the source promise's name. Each concrete instance has a separate result and counts separately in the totals. Its text result names the instance, and its JSON result adds an `instance` field with the concrete name and types. An unsupported instance refuses the selected set before initialization or tests run.

`mo test FILE --strength DIR` measures how many wrong versions the intent kills. It runs the program's own checks first, then each `.mo` file in `DIR`, which replaces named declarations of one module, against the same checks on the same fixed plan, and prints one `strength` record. Its rate counts the versions a promise or an example kills; a version that only a test or a recovery check kills is `killed_by_test`, counted in `killed_any` but not in the rate. A wrong version that stops is kept out of the rate, and so is one that does not check or changes a digest, unless a `counted` file in `DIR` lists it: a listed one stays in the rate as not killed. A plan cut short by `--seconds` exits 1 with E1432, "the plan did not finish".

`mo test FILE` runs every selected example, promise, test and recovery check, and exits 0 when they pass; each promise runs up to `--calls` generated inputs (200 by default) within `--seconds` (2 by default). `mo test FILE --mutants` is an opt-in diagnostic pass that runs every check with no time limit and then tries mutants of the program (reference 12.6). A mutant is one small change to the code of a function in `FILE` or in a program module it imports: a comparison moved to its boundary or negated, `and` swapped with `or`, a `not` removed, `+` swapped with `-` or `*` with `/`, a Bool flipped, or an Int raised by one. A library module's functions get mutants only when you run `mo test --mutants` on that module. Each mutant runs against the checks that ran its function, cheapest first, and is killed when one of them no longer passes or the program no longer starts. Each mutant that survives is an error, E1439, "a mutant survived: no check noticed this change", which names the function, the line and the change, and says when no check runs that function at all. A mutant that does not check is dropped. Before the totals line, a `mutants:` line counts the mutants tried, those killed by each kind of check and at start, those that survived and those dropped; when a check fails, no mutant is tried, and the line says `mutants: did not run: a check did not pass`. With `--json`, the totals carry a `mutants` object (or `null` when checks did not pass), and each mutant tried or dropped is a `mutant` record giving its function, place, kind and change, its `outcome` (`killed`, `survived` or `dropped`), the kind of check that killed it as `by` and that check's full name as `check`. `mo test FILE --mutants --list` runs nothing: after the checks' names it prints each mutant as `mutant FILE:LINE:COLUMN NAME: CHANGE` and each function of those modules as `function NAME: N mutants`, as `mutant` and `function` records with `--json`. `--mutants` takes no selector, `--calls`, `--seconds`, `--max` limit, `--bundle` or `--strength`.

Read `--mutants` for what it is: killing every mutant is a floor on how thoroughly the checks run the code, not evidence that they catch the mistakes agents make. Write checks for the mistakes you expect, and use `--mutants` as a diagnostic lens when auditing coverage rather than adding artificial examples to getters or contorting clean code.

`mo diff [OLD] NEW` checks how a program changed from `OLD` (or Git `HEAD` when only `NEW` is given) to `NEW`: capability authority (including `Files.within`, `ReadFiles.within`, `Files.read_only` and `Network.allow_hosts` narrowings), exposed signatures and service tools, saved state schema (`migration_required`), and behavioral contracts by running `OLD`'s promises, service promises and examples against `NEW`. Private helpers and constants in `OLD` that its contracts read are carried along when `NEW` removed or changed them, so a refactoring that deletes private helpers still verifies against `OLD`'s behavioral contract. It exits 0 when authority is not expanded and every contract passes, and 1 when authority expands or a contract fails or stops. With `--json`, it emits `authority`, `wire`, `state`, `contract` and `contracts` records.

The `lib/` folder holds library modules written in Mo, embedded into the `mo` binary so a single `mo` executable runs anywhere without an external `lib/` directory (`--lib DIR` overrides the embedded library with a folder on disk). Every `mo` command loads them with your program, `Http`, `Json` and `Unicode16` only when a `use` line names them. `mo check lib/list.mo` and `mo test lib/list.mo` check and test one of them on its own. `lib/list.mo` has `List`'s `map`, `filter`, `filter_map`, `flat_map`, `reduce`, `find`, `any?`, `all?`, `count`, `sort_by`, `sort_by_desc`, `sort`, `min`, `max` and `group_by`, `lib/map.mo` has `Map.update`, `lib/option.mo` has `Option.or_fail` and `Option.map`, `lib/result.mo` has `Result.map` and `Result.map_error`, `lib/file-error.mo` has `FileError.to_text`, `lib/json.mo` is the `Json` module (`use Json`, then `Json.parse`, `to_text` and `pretty`), with lookup by `get`, `at`, `path`, `text_at`, `int_at`, `bool_at` and `list_at`: `reply.text_at("items.0.user.login")` is `Some` of the string there, and `None` when the path is absent or holds another kind of value, and `lib/files.mo` and `lib/read-files.mo` have `walk` for `Files` and `ReadFiles`. `lib/text.mo` has no functions, only tests of `Text`'s operations, written from reference section 14 and run by `mo test lib/text.mo`. `lib/set.mo` likewise tests `Set`'s operations, `List.to_set` and Set text forms with `mo test lib/set.mo`. `lib/unicode16.mo` is the `Unicode16` module. After `use Unicode16`, its built-in `graphemes`, `display_width`, `normalize_nfc`, `lowercase` and `uppercase` operations follow Unicode 16.0. `mo test lib/unicode16.mo` tests them. `lib/time.mo` has no functions, only tests of `Time`'s, `Duration`'s and `Clock.fixed`'s operations, run by `mo test lib/time.mo`. The rest of section 14 is still built in. Section 10.5 names what is always in scope.

After `use Unicode16`, `Unicode16.letter?(c: Char) : Bool` tests Unicode 16.0 General_Category L (`Lu`, `Ll`, `Lt`, `Lm`, `Lo`), and `Unicode16.digit?(c: Char) : Bool` tests `Nd` (decimal digits). Neither includes `Nl`, `No` or marks, normalizes, or uses locale tailoring. For example, `text.chars.filter(fn(c) Unicode16.letter?(c) or Unicode16.digit?(c) end)` keeps letters and decimal digits.

After `use Json` (or `use Shop.Account`), a bare `Json` (or `Account`) in a type is the module's own type of that name, as reference 10.3 says: write `fn f(value: Json)`. `Json.Json` means the same type.

## Write this program

Use this shape.

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
- Bind a name once with `name = value`. A name is visible from its binding to the end of its block. To assign it again, declare `var name = value` where it is first bound, even when the assignment is inside a loop or a branch. Sections 6.1 and 6.2.
- Join texts by inserting them into one text, as in `"#{name}: #{score}"`. `+` adds numbers and does not join texts. Section 3.4.
- A list's length is `list.size`. A text's is `text.char_count` or `text.byte_count`. There is no `len` or `length`. Sections 3.4 and 14.
- Add an element with `items = items.append(x)`, and declare `var items` when you assign that name again. Sections 3.7 and 6.2.
- Mo has no exceptions, and there is no "unwrap". Return `Error(...)`, pass it on with `try`, and handle it with `case`. Where a failure truly cannot happen, write `stop` and the reason. Sections 8.2, 8.3, and 8.4.
- Cover every possible value in a `case`. On a choice, a whole arm cannot be `_` or a name, so list the cases. Inside a pattern, `_` or a name may stand for anything. Section 7.2.
- Pass a capability in only as a parameter. Make a fake only in tests, examples, and promises. Sections 11 and 12.4.
- Declare a `struct` or an `enum`. Records have no methods. Put functions in that module. Sections 4 and 5.4.
- To state that a function holds for every input, write a `promise` block with one or more `Bool` lines over the parameters and `result`; a text label after `promise` is optional, and `mo test` fails vacuous promises. Section 12.2.
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
- Section 9 is the six abilities (`Equal`, `Hash`, `Order`, `Text`, `Number` and `Json`), `where`, `to_text` on a type with `Text`, and `to_json` / `Json.decode[T]` on a type with `Json`.
- Section 10 is `expose`, `use`, and constants. The sample is one module.
- Section 11 is the capabilities besides `Args` and `Console`, and narrowing a capability.
- Section 12 is examples, promises, and tests. The sample has examples and a test, and no promise.
- Section 13 is commands and done, under the headings below.
- Section 14 is the catalog. Call library functions as that section lists them, through the type.

Section 15 names later work, and it changes no current rule.

The executable grammar in `grammar/mo.y` is the authority. Appendix B summarizes it.

## Run these commands

Source commands take the file of the program's main module. A recorded service can also start from its data folder. `mo check FILE` reports the program's errors, `mo run FILE [ARGS...]` runs it with the arguments after the file's name, and `mo test FILE` checks its examples, promises, tests and recovery checks. `mo emit FILE -o OUT` writes its bytecode. `mo serve FILE --port N --data DIR` runs its service. `mo format` and `mo fix` rewrite the files they are given.

Duration units accept singular and plural spellings, including `millisecond`.
`mo format` writes `1.minute` and `2.minutes` on direct integer literals, using
their numeric value even in hexadecimal or binary. It preserves other receivers
and qualified function values, such as `n.minute`, `(1).minutes`, and `Int.minute`,
because it does not infer types (reference 3.9).

`mo guide` needs no file. It prints this page. `mo guide --reference` prints the reference.

`mo` exits with status 0 when everything passed, 1 when the program has errors or an example, promise, test or recovery check fails, and 2 for a mistake in the command itself. Once the program runs, `mo run` exits with the program's own status, or 70 after a bug. Section 11.2 states the program's own status. Section 13.2 states that bug. `mo serve` has more statuses, listed under Run a service. `mo replay`, `mo query` and `mo history` exit 0 when the requested result holds, 1 when it does not or a check refused, and 2 for a mistake in the command; `mo query` exits 70 when its function stops, with the report `mo run` prints.

Section 13.4 names the selection flags and the limits.

## Run a service

`mo serve FILE --port N --data DIR [--allow-host NAME]` runs the service that the module in `FILE` declares. Check three things first:

- `FILE` has a `service` block. Without one, `mo serve` refuses with E1409, "this module has no service to start". An imported service does not count.
- `DIR` exists. `mo serve` never creates it, so a mistyped path cannot start an empty service. Saved state goes in `DIR/service.sqlite`. `service.sqlite` is readable only by its owner. Only one `mo serve` can use a `DIR` at a time.
- The environment variable named by `token from env "NAME"` holds the access token. A token is 1 to 256 bytes of letters, digits and `-._~+/`, and may end in `=` padding. A missing or invalid token is E1409, and the message names the variable.

`--port` takes 0 to 65535. Port 0 picks a free port. `mo serve` listens on 127.0.0.1 only. It takes no address flag. `--allow-host NAME` also accepts requests whose `Host` is `NAME`, as a proxy in front of the server sends. A `NAME` without a port means port 80. A malformed `NAME` is E1409, "the server configuration was refused". The flag never changes which saved data the program recovers.

The requests below call this service, saved as `counter.mo`:

```text
module Counter

service
  state Int = 0
  tools add, read
  token from env "COUNTER_TOKEN"

  promise on add
    after == before + amount
    reply == after
  end

  promise on read
    reply == before.to_text
  end
end

## Adds an amount to the count.
fn add(state: Int, _now: Time, amount: Int) : (Int, Int)
  (state + amount, state + amount)
end

## Reads the count.
fn read(state: Int, _now: Time) : Text
  state.to_text
end
```

When it is ready, it prints one line to standard output:

```text
mo serve: listening on http://127.0.0.1:PORT/mcp
```

Send each request with `POST` to that URL. One connection carries one request. Use the host `127.0.0.1:PORT`, `localhost:PORT` or the `--allow-host` name. Any other well-formed `Host` gets status 421, and a malformed or missing one gets 400. A missing or wrong token gets status 401. Start with `initialize`:

```sh
curl -s http://127.0.0.1:PORT/mcp \
  -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' \
  -H 'MCP-Protocol-Version: 2025-11-25' \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"demo","version":"1"}}}'
```

Send the same headers with each of these bodies. The first gets status 202 and no body. The last two call a tool:

```json
{"jsonrpc":"2.0","method":"notifications/initialized"}
{"jsonrpc":"2.0","id":2,"method":"tools/list"}
{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"add","arguments":{"amount":"5"}}}
{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"read"}}
```

An `Int` argument is a JSON string, as `tools/list` shows in each tool's `inputSchema`. A tool's reply is in `result.structuredContent`. A failed call has `isError` true and `structuredContent.error.code`, such as `handler_failed` or `store_failed`.

`mo serve` writes one line to standard error for each failure, restart and stop, in exactly these formats:

```text
mo serve: failure: NAME: REASON
mo serve: restart: N of M: REASON
mo serve: stop: REASON
```

`NAME` is the tool or timer that failed, as the source writes it, such as `halt` or `tick`. A bad interval names its timer. A saved message that replay runs and rejects, because its handler failed or its result failed the checksum, names its tool or timer. A record refused before it runs, because it is malformed, out of sequence or from a different program, is `service`. `NAME` is the word `service` when no tool or timer is responsible, as when the state's initial value fails, or the store or saved data fails at startup. `mo check` refuses `service` as a tool or timer name, so the word never means a real handler. `NAME` never holds a colon, and `REASON` is always one line. A store that will not open is E1409 with no `failure` line. A store another process holds, such as a second `mo serve` or a reader of `DIR`, is not E1409: `mo serve` waits up to 10 seconds for it, then prints one `mo serve: could not open saved state at` line and exits 75. `N` is this restart's place among the restarts of the last ten minutes, and `M` is the limit of 3, so `3 of 3` is followed by the stop line. A store failure while the service runs is a `stop` line with no `failure` line. A failure that is a bug adds the report `mo run` would print after its line, except during replay. The report's values can quote these words, so anchor a search as `^mo serve: failure:`. Read journald with `journalctl -o cat`, so each line keeps its start. The lines have no timestamps. These line shapes are stable, and changing one is a breaking change. The words of `REASON` are not stable, so branch on the line's kind, on `NAME` and on the exit status. The token never appears in output, logs or errors.

`mo serve` exits with one of these statuses:

- 0: SIGTERM or SIGINT stopped it. It closes its port, so a new connection is refused, and finishes the open ones first, which their deadlines bound to about 25 seconds.
- 1: the program has errors, or it could not start (E1409).
- 2: a mistake in the command.
- 69: supervision (D308) stopped the service. Five failures within a minute restart it, and three restarts within ten minutes stop it. It stays down until a person starts it, as Recover from a stop says.
- 70: it died any other way, such as a failed save, a listener failure or a bug in a module constant. Starting it again is safe.
- 75: another process held the store for the whole wait. Nothing was changed, and starting it again is safe, so a service manager should retry it.

`DIR` records the canonical source bundle, its digest, and the build and state identities alongside the saved state. The first `mo serve FILE --data DIR --port N` records the program atomically with its initial snapshot. Thereafter `mo serve --data DIR --port N` compiles the recorded program and starts it. Supplying `FILE` requires the same source digest; any source-byte change, including comments, needs an explicit upgrade. Moving the same files does not change their source identity. A new executable build must verify every replayed state checksum before recording its build identity, even if the service image key is unchanged. The previous program identity is retained so failures captured under either build remain attributable. A recorded program that no longer compiles is refused; keep the recording executable for recovery.

A legacy folder with no recorded program can be adopted when its compiled image matches. For a different image, `--adopt` explicitly permits verified replay. An empty tail verifies zero messages and provides no evidence about changed handler behavior. Adoption is not a substitute for migrating an incompatible state schema.

## Recover from a stop

After status 69, read the `mo serve: failure:` lines and their reports to find the handler that failed. Stop callers that repeatedly send the failing message before restarting.

- Restart the recorded program with `mo serve --data DIR --port N`.
- Check a candidate with `mo check NEW --upgrade-from OLD [--migrate Module.function] [--json]`. `OLD` can be a standalone source file or a stopped service data folder. Use the folder after a prior migration so the check can read its recorded source and validated generated declarations. The check does not write to the folder. Exit 0 means the source and migration signature checks passed; it does not mean a migration ran over the folder's state.
- With the service stopped, run `mo serve NEW --data DIR --port N --upgrade [--migrate Module.function]`. Matching saved schemas need no migration. A changed schema needs a pure, non-generic function taking exactly one old state and returning exactly the new service state. Import generated old declarations from `UpgradeOld`; they come from the recorded program, never a candidate disk module. Changed access clauses are refused.

The checked restart runs the same checks as a push, refusing a failure with E1433 and an unfinished plan with E1432. Every refusal leaves the old program active. The checked restart replays the old state, runs the migration within restart budgets, validates the result and records the new program and snapshot in one activation transaction. A refused migration leaves the old program and state authoritative. After a successful activation, folder-only startup runs the new program without rerunning the migration. Generated old declarations needed to compile it are retained with the program separately from its source identity, including across later tool-only upgrades. Use it for a migration the live budget refuses.

Source-only `mo check` and `mo test` do not automatically load a recorded program's generated `UpgradeOld` declarations. Use a data-folder base for upgrade checks. `mo replay --failure ID --data DIR` without `FILE` replays a failure under the program the folder retained for it, chosen by image key, build and schema, with its generated declarations. When retained programs with different sources match, it refuses with E1431; give `FILE` then.

## Upgrade a running service

Start the service with an operator Unix domain socket: `mo serve --data DIR --port N --admin-socket PATH`. The socket is created with mode `0600` so OS filesystem permissions authenticate operator access; public HTTP (`--port N`) never exposes `mo-*` views or upgrade tools.

On the service host, or through an SSH Unix-socket forward (`ssh -L ./admin.sock:/run/service/admin.sock`), run `mo push NEW --admin-socket PATH [--base DIGEST] [--migrate Module.function] [--json]`. When `--base` is omitted, `mo push` reads the active program's source digest from `mo-overview`; a push naming a stale `--base` is refused with E1416 naming the active digest. The push uploads the source bundle, which the service compiles in a child process while it keeps serving. A second child process runs every promise, service promise, example and test in the modules the bundle carries under a fixed plan: 200 inputs per function promise and 1,000 sequences per service, with no clock cutting any check short: only the whole run's limits, 120 seconds of CPU and 180 seconds in all, stop it. A failure is refused with E1433, naming the failing item, and a plan that does not finish with E1432. Otherwise the service checks the candidate against the running program as the checked restart does, migrates the current state under the live budget (1,048,576 work units, 32 MiB), and activates it between two messages in one transaction. Queued calls continue under the new program in order; a queued call to a tool the candidate removed or retyped is answered with the runtime error `removed_by_upgrade` and has no effect; a queued tick of a removed timer is dropped. Timers keep their next due time by name. Exit 0 means the service runs the candidate; the report gives the activation `ordinal`. Pushing the active program again reports `already active` and changes nothing, so a push whose reply was lost is safe to repeat. Exit 1 means a refusal or client failure; exit 2 means invalid command usage or base digest. A refusal names its E14xx code and leaves the old program serving. After activation there is no rollback: a fault in the new program is fixed by another push or, with the service stopped, a checked restart.

The compile children run confined where the host can confine them: Linux with Landlock ABI 3 or later (kernel 6.2) and seccomp. Each child reads only the `mo` executable, `lib/`, the system libraries and its inputs, writes only its own folder, opens no socket and signals no other process. On a host that cannot confine, such as macOS, `mo serve` accepts no upgrade: started with `--admin-socket`, it prints `mo serve: upgrades: refused, because this host cannot confine the compile children; --unconfined accepts upgrades unconfined` on standard error, and a push, or `mo serve NEW --upgrade` with changed source, is refused with E1438. Add `--unconfined` to accept upgrades there anyway. The start line is then `mo serve: upgrades: the compile children are not confined (--unconfined)`, a JSON report adds `"confined":false`, and a text report adds the line `the compile children were not confined (--unconfined)`. Where the host can confine, `--unconfined` changes nothing.

Initialize returns an `MCP-Session-Id`. A client that sends it on `GET /mcp` with `Accept: text/event-stream` receives `notifications/tools/list_changed` after each activation; each session has one stream, and a new GET replaces it. Notifications are not a durable log: a reconnecting client calls `tools/list`. POST calls work with or without the session header.

If a checked restart is refused and the program cannot be repaired within those checks, use the fresh-folder fallback. Start the old program on the old `DIR`, read what you need through its tools, and stop it. Start the fixed program on a new, empty `DIR` and send that data to its tools. State that no tool returns stays behind, so keep the old source, executable and data folder.

## Operate through an admin socket forward

When operating a remote service over SSH Unix-socket forwarding (`ssh -L ./admin.sock:/run/service/admin.sock`), you can call operator views, push with `mo push`, query live state or failures directly with `mo query FILE FUNCTION --admin-socket ./admin.sock`, fetch an export with `mo fetch`, and replay or query a fetched export with the same `mo` build and the deployed source. `mo history`, replay and query on the data folder, the journal's failure reports, and anything after the service stops need shell access on the host.

## Work in this loop

Follow these steps.

1. Write the program. When a function has `examples` or a `promise`, put each block at the top of the body, then a blank line, then the code. Add a `test` in the same file when you check a scenario. Section 12.
2. Run `mo format` on those files.
3. Run `mo check FILE`. It reports each error it finds, in source order. Fix every error it reports before you run `mo check` again, not only the first. Handle E1103 and E1150 as Know this build says.
4. Run `mo fix FILE` when `mo check` shows a fix. When an error shows more than one fix, choose one and make that edit yourself. Appendix A.
5. Run `mo test FILE` only once `mo check` reports no errors but those Know this build names. While the program has other errors, `mo test` runs nothing and prints the same errors `mo check` does. Run `mo test FILE` until the examples, promises, tests, and recovery checks pass. Use `mo test FILE --mutants` when you want to audit which code changes your checks do not yet notice (E1439).
6. Run `mo run FILE` only when the task needs a running program.

If a command exits 1 for a reason that Know this build names, follow its instructions above, and continue. Section 13.4 describes `--json`.

## Stop when the task is done

The task is done when the program is the v0.1 Mo the reference describes for what it computes, including the specs and tests that behavior needs, and you have run the loop steps this build implements.

On a complete toolchain, `mo format`, `mo check`, and `mo test FILE`, with no selector or limit, exit 0. Run `mo run` when the task is a running program.

On this build, accept a remaining diagnostic only when Know this build names it: E1103 or E1150 on a form the reference requires and offers no other way to write the same behaviour, or E1400 on a promise whose inputs `mo test` cannot build. Deleting a required spec, promise or capability to silence a diagnostic leaves the task undone. Deleting a promise because `mo test` returns E1400 leaves the task undone.

E1408 is also an expected build limitation when `mo run` targets a service entry module. Use `mo serve` to run the service. Use `Simulation` in tests for scheduling and persistence, or test ordinary handlers directly for their value logic. Simulation provides no networking or deployment.

Where the reference says nothing, do not add a rule, and do not fill that silence from the architecture or from `formal/TRANSLATION.md`.

Section 12 shows the form of a spec. When the task is a program, read section 11.2 for `main`'s result and the program's exit status.

### Fetch a private state artifact on the service host

Run `mo fetch --admin-socket /run/service/admin.sock --export state --out state.moex`
on the host running the persistent service, or through an SSH Unix-socket forward. The destination must not exist (including a symlink).
The command connects over the Unix domain socket without proxy or redirect handling.
It verifies one complete export generation and publishes a mode-0600 artifact. A failed or
interrupted attempt removes its temporary file; retry creates a new generation. The largest
state requires 2049 pages, about 103 seconds at the service's call rate limit. The largest
failure pair requires 2176 pages, about 109 seconds.
Pin creation, reading the final page (which releases the pin), and publishing the verified local
file are separate events. Repeated creation may observe different state. Use `mo-failures` to
list retained diagnostics and `--export failure ID` to fetch a retained failure pair with the same
verification and private-file publication rules.

### Diagnose a failure offline

These commands run on the service host and start no service. Run them as the service's user, or
first copy the whole stopped data folder, including any `service.sqlite-journal` beside the
database. Never copy `service.sqlite` alone.

`mo history DIR` shows what a stopped data folder holds, and runs no Mo code and no timer. It
shows the last snapshot "as of sequence S, K later messages not applied", the log tail's
sequence, handler index, arrival and argument sizes, and each retained failure with its build and
whether this build can replay it. The log keeps a handler's index, not its name, so a tail entry
says "name not recorded in the log". Messages before the snapshot are gone, so a message missing
from the tail may still have run. A store from before failure capture says "this store predates
failure capture". `mo history` refuses at once, with its own message, a folder a running service
owns, a folder it cannot read, and a folder with no saved data. A journal left by an unfinished
write needs a rollback: run it again as the folder's owner with `--recover`, which writes
database pages but changes no record.

`mo replay FILE --failure ID --data DIR` runs the failed handler again on its recorded state,
arrival time and arguments; `--export failure.moex` reads a fetched artifact instead of `DIR`. It
prints the original message, the pre-call state, the recorded and offline signatures and the
regenerated stack with the top frame's variables. `FILE` must be the untouched source the service
ran, so use the deployed `/opt/mo/$SHA` tree: any byte change, a comment or `mo format` included,
changes the program identity and is refused. Replay also needs the `mo` executable that recorded
the failure: `mo history` shows each record's build digest, and
`sha256sum /opt/mo/*/build/gcc/debug/mo` finds the executable with that digest. Records from a
build without `mo replay` cannot be replayed. The
outcome is `reproduced` (exit 0), or `different`, `not_reproduced` or `refused` (exit 1). A memory
failure that depended on arguments queued in the mailbox does not recur offline, and the outcome
says so.

To check a fix, copy the program's folder, edit the copy, and add `--fix COPY/main.mo`. Both
programs are checked before anything runs, so a fix that changes the state type, or the handler's
name, kind, arguments or result shape, is refused. The original replays first, then the revised
handler runs on the same inputs and budgets. The verdict is `verified` (exit 0) only when the
original reproduced and the revision succeeded within the recorded budgets; otherwise it is
`not_fixed`, `unverified` or `refused` (exit 1). `verified` says the failure is gone, not that the
revision does the right work: the revised outcome shows its reply kind and whether the state
changed.

`mo query FILE FUNCTION --export state.moex` (or `--admin-socket PATH`) runs a function of yours over an exported or live state:
`FUNCTION(state)`. `mo query FILE FUNCTION --failure ID --data DIR` (or `--export failure.moex` or `--admin-socket PATH`)
runs it over a failure's inputs: `FUNCTION(state, time, arguments...)` in the handler's order.
`FUNCTION` is a function of `FILE`'s main module, or `Module.name`, exposed or not. `FILE` may add
functions or modules to the deployed program, but the state type must not change, and the input
must come from this `mo` build. The function is not generic (E1410), holds no capability or
function in its parameters or result (E1411), takes the main service's state type first (E1412)
and then exactly the inputs above (E1413). A query runs under `--max-work N` instructions, by
default 1073741824, and `--max-heap`, `--max-stack` and `--max-depth` as `mo run` does; a query
that stops on one exits 70 with a report that names the limit. The result prints as structural
text, or as JSON with `--json`.

`--recover` on `mo replay` and `mo query` means what it means on `mo history`: run as the
folder's owner, it rolls back a journal left by an unfinished write, which writes database pages
but changes no record. It goes with `--data DIR`, never with `--export`.

A refusal prints one `mo replay: refused: CHECK: ...` line, or `mo query: refused:`, naming what
differs with its recorded and found values, and exits 1. No handler or query function runs
before every check passes. The checks run in a fixed order: the input, its build, the program and its
handler, the types, then a fix and the budgets; only the first that fails is named. `mo query`
does not check the program. `CHECK` names are stable; the words after them are not, so branch
on `CHECK`.

| `CHECK` | What differs | What to do |
| --- | --- | --- |
| `format` | The artifact or record cannot be read, is not a regular file, or its state does not belong to its message | Fetch the artifact again, or copy the stopped folder again |
| `kind` | A state export given with `--failure`, or a failure without it | Fetch the other kind, or add or drop `--failure ID` |
| `folder` | The data folder cannot be read, as `mo history` would say | Read its message; `--recover` for a journal |
| `failure` | No record with that ID: the folder lists the IDs it keeps, an artifact holds another | Pick a listed ID from `mo history DIR` |
| `build` | Another `mo` build recorded it, or the record names no build | Run the `mo` whose digest `sha256sum` matches |
| `program` | `FILE` is not the untouched deployed source, or declares no service | Use the deployed `/opt/mo/$SHA` tree |
| `handler` | `FILE`'s service has no handler of the recorded name, kind and arguments; for `mo query`, also no service | Use the deployed source, or keep the handler in `FILE` |
| `run` | No memory for the checks | Free memory and run again |
| `state_schema` | `FILE`'s state type differs from the recorded state's | Keep the deployed state type in `FILE` |
| `argument_schema` | A handler or query argument's type differs from the recorded one | Match the handler's argument types |
| `revised_program` | `REVISED` does not compile (its diagnostics come first) or declares no service | Fix it until `mo check REVISED` passes |
| `revised_state_schema` | The fix changes the state type | Keep the state type |
| `revised_handler` | The fix renames the handler or changes its kind, result shape class or arguments | Keep the handler's name and shape |
| `budgets` | The record's budgets differ from this build's, so it is damaged or crafted | Copy or fetch the record again |

Parse `--json`, not text: a text refusal goes to stderr, and `--json` writes every record to
stdout. `--json` writes a version record, one record per outcome (`original`, `revised`, `query`
or `history`), and a totals record whose `status` is the exit status. The fields to branch on are
`outcome` on `original` (`reproduced`, `different`, `not_reproduced`, `refused`) and on `query`
(`returned`, `refused`, `stopped`, with `limit` naming the limit); `verdict` on `revised`;
`refusal.check`, `refusal.recorded` and `refusal.found` on a refused check; `run.status` and
`note` on `original`, where `note` is non-null only when a memory failure did not recur; and, on
`history`, `folder` (`ok`, or why not, as `owned` or `recovery_required`), each failure's `id`
and `replayable_here`.

### Rehearse an upgrade offline

`mo rehearse --artifact EXPORT --old-bundle OLD --candidate NEW
[--migrate Module.function] [--context OLD.mocc] [--json]` reads a state export
and two canonical MOSB source bundles. The export must come from this exact
build; OLD must compile to its recorded program, service and saved schema.
For a program retaining an earlier migration body, supply its recorded MOCC
compilation context with `--context`; original source identity remains separate.
The candidate uses the same typed comparison and migration runner as checked
restart, with the smaller live migration work and heap limits. Exit 0 means
"would be accepted" for this snapshot; exit 1 means refusal. The JSON report
includes `rehearsal: true`, `accepted: false` and `state_valid`.
Rehearsal is not upgrade acceptance. It writes no export or service data and
opens no data folder; compilation uses temporary scratch files. A future live
upgrade must repeat admission against the then-current service state.
