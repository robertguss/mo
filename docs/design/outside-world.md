# Mo talks to the outside world

Status: proposed on 2026-11-17 as Phase 7's design. Robert has not approved it, and nothing in
it is built. Every choice below that is his to make is in
[Decisions for Robert](#decisions-for-robert), each with a recommendation. The evidence, with its
commands and raw outputs, is in [the preflight](../../learnability/phase7/preflight/README.md).

A Mo program gets two new capabilities: the network and a SQLite database. An HTTP client ships
in `lib/`, written in Mo on top of the network capability. A program can look through a JSON
response without declaring types first. The children that `mo serve` starts to compile and check
pushed source run under operating-system confinement.

One measurement missed the bar that was written down before it. It is the first decision in the
list, and the level of the network capability is not fixed until Robert has made it.

## A spike in Mo

Both programs type-check today against stand-in modules
([surface/](../../learnability/phase7/preflight/surface/README.md)). Nothing in them runs yet.

### The first run: look at a response

```mo
## Prints one API response readably: the first run of a spike.
##
## The token is read from the environment variable API_TOKEN.
module Look
use Http{HttpError}

fn main(env: Env, console: Console, network: Network) : Result(Unit, HttpError)
  url = "https://api.example.com/repos/mo/mo/issues"
  token = env.get("API_TOKEN") or ""
  call = Http.get(url).query("state", "open").header("Authorization", "Bearer #{token}")
  response = try call.send(network)
  console.print("#{response.status}")
  issues = try response.json
  console.print(issues.pretty)
  Ok(Unit)
end
```

What it shows:

- The program reaches the network only because `main` lists `Network`. A program that does not
  list it has no value to pass to `send`, so `mo check` rejects it.
- The client is a library that ships with `mo`. Nothing is installed.
- One call carries a method, a query, a header and a timeout (30 seconds unless the program says
  otherwise). The status is a number on the response. A failure is a value: `try` hands it to
  `main`, which prints it and exits with status 1.
- A response whose shape the program does not know is printed readably with no type declared.

### The tool: take fields out, save rows, run it again

```mo
## Saves a repository's open issues in a SQLite database and prints their titles.
##
##   issues OWNER/REPO
##
## The token is read from the environment variable API_TOKEN.
module Issues
use Http{HttpError}

enum Halt
  Usage
  Call(HttpError)
  Database(SqliteError)
  Unexpected(Text)
end

fn main(args: Args, env: Env, console: Console, network: Network, files: Files) : Result(Unit, Halt)
  repo = try args.list.first.or_fail(Usage)
  database = try files.sqlite("issues.db")
  saved = try sync(network, database, repo, env.get("API_TOKEN") or "")
  console.print("saved #{saved} open issues")
  found = try database.query("select number, title from issues order by number", [])
  for row in found.rows
    console.print("#{row}")
  end
  Ok(Unit)
end

## Fetches the open issues of `repo` and saves each one. Gives the number saved.
fn sync(network: Network, database: Sqlite, repo: Text, token: Text) : Result(Int, Halt)
  url = "https://api.example.com/repos/#{repo}/issues"
  call = Http.get(url).query("state", "open").header("Authorization", "Bearer #{token}")
  response = try call.send(network)
  return Error(Unexpected("the API answered #{response.status}")) if not response.ok?
  issues = try response.json
  try database.run(
    "create table if not exists issues (number integer primary key, title text not null)",
    []
  )
  var saved = 0
  for issue in issues.list_at("") or []
    number = try issue.int_at("number").or_fail(Unexpected("an issue has no number"))
    title = try issue.text_at("title").or_fail(Unexpected("issue #{number} has no title"))
    try database.run(
      "insert or replace into issues (number, title) values (?, ?)",
      [Sqlite.Integer(number), Sqlite.String(title)]
    )
    saved += 1
  end
  Ok(saved)
end
```

What it shows:

- Values come out of an unknown response by path: `int_at("number")`, `text_at("title")`.
- The program opens a SQLite database through `Files`, a capability `main` lists. Statements
  take their values as parameters, and rows come back as Mo values.
- It runs as a command-line tool: `mo run issues.mo mo/mo`, with arguments and an exit status
  as programs have today.

### A test with no network

```mo
test "sync saves each open issue, with no network"
  page = "[{\"number\": 7, \"title\": \"Crash on empty input\"}, {\"number\": 9, \"title\": \"Slow start\"}]"
  url = "https://api.example.com/repos/mo/mo/issues"
  asked = Http.get(url).query("state", "open").header("Authorization", "Bearer t0ken")
  network = Network.fake(Http.replies([(asked, Http.reply(200, page))]))
  database = Sqlite.in_memory()
  assert sync(network, database, "mo/mo", "t0ken") == Ok(2)
  assert database.query("select title from issues order by number", []) is Ok(found)
  assert found.rows == [[Sqlite.String("Crash on empty input")], [Sqlite.String("Slow start")]]
end

test "a request the fake holds no reply for is an error"
  network = Network.fake(Http.replies([]))
  assert sync(network, Sqlite.in_memory(), "mo/mo", "t0ken") is Error(Call(_))
end
```

What it shows:

- The fake network answers from the responses the test supplies. The test reads as a request
  and a response and never as raw bytes.
- A request the fake holds no reply for is an error the program receives, and no real
  connection is made.
- The database in the test is in memory. Only tests, examples and promises can make one.

## The network capability

The capability works at the level of a connection. HTTP is Mo code on top of it. TLS, when it
is added, sits under the connection, so one Mo client serves HTTP and HTTPS.

Two types are new. `Network` is the capability `main` lists. `Connection` is one open connection.
A program gets a `Connection` only from a `Network`, and `main` cannot list one.

| Operation | Signature | What it does |
| --- | --- | --- |
| `network.connect(host, port, within)` | `fn(Network, Text, Int, Duration) : Result(Connection, NetworkError)` | Looks up `host`, and opens a TCP connection to `port`. `within` is one deadline for the whole life of the connection: the lookup, the connect, and every send and receive after it. |
| `connection.send(bytes)` | `fn(Connection, Bytes) : Result(Unit, NetworkError)` | Sends all of `bytes`. |
| `connection.receive(most)` | `fn(Connection, Int) : Result(Bytes, NetworkError)` | Waits for data and gives at most `most` bytes of it. Empty `Bytes` means the other side has finished. |
| `Network.fake(peers)` | `fn(Map(Text, Map(Bytes, Bytes))) : Network` | A network that answers from `peers`. Only tests, examples and promises may call it. |

A connection has no `close`. It closes when the program drops its last reference to it, as a
folder from `files.within` does. One connection carries one request in this phase, so one
deadline is enough.

The HTTPS milestone adds one operation, `network.connect_secure(host, port, within)`. It gives
the same `Connection` with TLS under it, and verifies the server's certificate against `host`.
It also adds one error case for a certificate that fails.

```mo
enum NetworkError
  UnknownHost(host: Text)
  Refused(host: Text, port: Int)
  TimedOut
  Closed
  Broken(reason: Text)
end
```

`Closed` is a connection the other side reset, or closed before a send finished. `Broken` carries
the operating system's reason for anything else.

These stop the program as bugs, because no input can make a correct program do them: a port
outside 1 to 65,535, a `most` below 1 or above the receive limit, and more open connections than
the limit.

### The fake

`Network.fake(peers)` takes a map. Its key is `"host:port"`. Its value is a map from the exact
bytes a connection sends to the bytes it then receives.

- `connect` to a key the map lacks gives `Error(UnknownHost(host))`.
- What a connection sends is collected. The first `receive` looks up everything sent so far.
- With no entry, `receive` gives `Error(Broken(...))`, and the reason quotes the first line of
  what was sent, so a test that fails says which request had no reply.
- With an entry, `receive` hands out the reply, at most `most` bytes at a time, and then the
  end.
- A fake never opens a socket and never reads a clock. `within` is ignored.

A test does not write these bytes. `Http.replies` builds the map from requests and responses
(below). The client writes a request the same way every time, so the match is exact: a test
states the whole request, headers included, and a program that sends anything else gets an error.

The fake's state is the map, and for each connection the bytes sent and a position. That is
within the rule for fake state in `docs/design/bytecode.md`: numbers, texts, bytes, and lists,
sets and maps of them.

### Limits

Named in `src/base/limits.h`, with these starting values:

| Limit | Value | When passed |
| --- | --- | --- |
| Connections open at once | 64 | Bug |
| Bytes in one `receive` | 1 MiB | Bug |
| Length of a host name | 253 bytes | `Error(UnknownHost(host))` |

## `Bytes` operations

`Bytes` has `size`, `get` and `decode_utf8` today, and nothing that builds or cuts. The client
needs two more, and the prototype measured them:

| Operation | Signature | What it does |
| --- | --- | --- |
| `bytes.slice(from, to)` | `fn(Bytes, Int, Int) : Bytes` | The bytes from index `from` up to, not including, `to`. An index past the end is the end, as with `List.take`. A negative index is a bug. |
| `Bytes.join(parts)` | `fn(List(Bytes)) : Bytes` | All the parts, in order, as one value. |

Without them the same fetch copies each byte into a list. It takes 2.5 times the instructions
and up to 2.3 times the heap.

Both are primitives: rows in `src/base/prims.def`, rows in `docs/design/library.md`, and Lean
counterparts with their safety proofs. They go at the end of `prims.def`, because primitive
numbers are positions in that file and four of them are pinned in `src/prim/prim.c`.

## The HTTP client

The client is the library module `Http`, in `lib/http.mo`. It is written new, on `Bytes`.

`runtime/http.mo` already declares `module Http`. It is the server-side header parser that
`mo serve` uses, it works on `List(Byte)`, and its limits are sized for MCP. A user program
cannot `use` a module in `runtime/`. The protocol server's program is built from `runtime/` and
`lib/` together, so two modules called `Http` cannot both exist, and the runtime's module is
renamed (to `HttpServer`). The two parsers stay separate in this phase.

```mo
struct Request
  method: Text
  url: Text
  headers: List((Text, Text))
  body: Bytes
  timeout: Duration
end

struct Response
  status: Int
  headers: List((Text, Text))
  body: Bytes
end

enum HttpError
  BadUrl(url: Text)
  Unreachable(NetworkError)
  BadResponse(reason: Text)
  NotJson(JsonError)
  TooLarge(limit: Int)
end
```

| Function | Gives | Notes |
| --- | --- | --- |
| `Http.get(url)`, `Http.post(url)` | `Request` | |
| `Http.request(method, url)` | `Request` | Any other method. |
| `request.header(name, value)` | `Request` | Adds a header. |
| `request.query(name, value)` | `Request` | Adds a query parameter, percent-encoded. |
| `request.json(value)` | `Request` | Sets the body to `value` as JSON, with its content type. |
| `request.text(text)` | `Request` | Sets the body to `text`, with its content type. |
| `request.within(timeout)` | `Request` | Replaces the 30 seconds a request starts with. |
| `request.send(network)` | `Result(Response, HttpError)` | Makes the request. |
| `response.ok?` | `Bool` | The status is from 200 to 299. |
| `response.header(name)` | `Option(Text)` | The name is matched without regard to case. |
| `response.json` | `Result(Json, HttpError)` | The body parsed as JSON. |
| `response.text` | `Option(Text)` | The body as text, when it is UTF-8. |
| `Http.reply(status, body)` | `Response` | For tests. |
| `Http.replies(exchanges)` | `Map(Text, Map(Bytes, Bytes))` | For tests: what `Network.fake` takes, from a list of `(Request, Response)`. |

What `send` does:

- It speaks HTTP/1.1, opens one connection for each request and asks the server to close it.
- It reads a response with `Content-Length`, with chunked transfer encoding, or ended by the
  close of the connection.
- A status of 404 or 500 is a response. It is not an error.
- It follows no redirect, retries nothing, keeps no cookie, uses no proxy, reuses no
  connection, and asks for no compression. Robert's spike shows which of these he misses.
- A URL is `http://` or `https://`, a host, an optional port, a path and a query. Anything else
  is `BadUrl`. Until the HTTPS milestone is built, an `https` URL is an error that says so.
- The response's headers may take 64 KiB and its body 16 MiB. Past either, the result is
  `BadResponse` or `TooLarge`. Both are constants in `lib/http.mo`.

All parsing of what the network sends is Mo. The only new C that reads network bytes is the
`receive` operation, which copies them into a `Bytes` and inspects nothing, and later the TLS
library. Each gets a fuzz target with seeds.

## Looking through a response

`lib/json.mo` parses and pretty-prints today. It gains lookup:

| Function | Gives | Notes |
| --- | --- | --- |
| `value.get(key)` | `Option(Json)` | A field of an object. |
| `value.at(index)` | `Option(Json)` | An item of an array, counted from 0. |
| `value.path(steps)` | `Option(Json)` | Steps separated by `.`, as in `"items.0.user.login"`. |
| `value.text_at(steps)` | `Option(Text)` | The string at that path. |
| `value.int_at(steps)` | `Option(Int)` | The whole number at that path. |
| `value.bool_at(steps)` | `Option(Bool)` | |
| `value.list_at(steps)` | `Option(List(Json))` | The items of the array at that path. |

A step is a key when the value is an object and an index when it is an array, so an object
whose keys are digits still works. The empty path is the value itself, which is how the tool
above reads a response that is an array at the top. A key that contains a `.` cannot be written
in a path. `get` reaches it.

Every lookup gives `None` when the path is absent or the value has another kind. None stops the
program.

`examples/json-query.mo` has this lookup today, beside its own copy of the `Json` type from
before the library existed. It becomes a short program that uses the library.

## The SQLite capability

A program opens a database through `Files`:

| Operation | Signature | What it does |
| --- | --- | --- |
| `files.sqlite(path)` | `fn(Files, Text) : Result(Sqlite, SqliteError)` | Opens the database file at `path`, and creates it when it is absent. |
| `Sqlite.in_memory()` | `fn() : Sqlite` | A database in memory. Only tests, examples and promises may call it. |
| `database.run(statement, values)` | `fn(Sqlite, Text, List(Sqlite.Value)) : Result(Unit, SqliteError)` | Runs one statement and gives no rows. |
| `database.query(statement, values)` | `fn(Sqlite, Text, List(Sqlite.Value)) : Result(Sqlite.Rows, SqliteError)` | Runs one statement and gives its rows. |

```mo
enum Value
  Null
  Integer(Int)
  Real(Float)
  String(Text)
  Blob(Bytes)
end

struct Rows
  columns: List(Text)
  rows: List(List(Value))
end

enum SqliteError
  OutsideFolder(path: Text)
  NotPermitted(reason: Text)
  SqlFailed(message: Text)
end
```

`Value` has the five kinds SQLite stores. A program writes `Sqlite.Integer(7)`, with the module's
name in front, as it writes `Json.Array(items)`. Each `?` in a
statement takes the next value in the list. A count of values that does not match the statement
is `SqlFailed`. `Value` has a text form, so a row can be printed.

### Authority

The authority is the folder that the `Files` reaches. A program that holds a `Files` can
already read and write every file in that folder, so a database in it adds nothing. There is no
second rule about paths: `path` is checked as every `Files` path is, and one that leaves the
folder gives `Error(OutsideFolder(path))`. SQLite's journal files sit beside the database, in
the same folder.

A `Sqlite` holds the database it opened and can open no other file:

- `ATTACH` and `VACUUM INTO` are refused with `NotPermitted`.
- Extension loading is already left out of the build.
- Temporary storage stays in memory.

Without the second half of that, a test in pushed source could make a database in memory and
use it to read or write the host's files from inside the check child.

Everything else SQLite does by default is allowed: triggers, views, transactions written as
statements. The service store's own rules stay the store's.

`ReadFiles` gets no `sqlite` in this phase.

One hole is known. SQL can read the clock (`datetime('now')`) and draw random numbers
(`random()`), and the program needs no `Clock` or `Random` to do it. It is on the list of
decisions.

### How it is built

The capability uses the SQLite already linked into every `mo`, and its code sits beside the
service store in `src/host/`. It shares the store's allocator setup, which is made once for the
process under one memory budget. A `Sqlite` owns its database handle and closes it when the
program drops the last reference.

| Limit | Value | When passed |
| --- | --- | --- |
| Databases open at once | 16 | Bug |
| Length of one statement | 1 MiB | `Error(SqlFailed(...))` |
| Values in one statement | 32,766 | `Error(SqlFailed(...))` |

A query's rows are Mo values, so the program's heap limit bounds a result.

## Confinement of the children

`mo serve` starts three child runs for each upgrade it is sent: the compile, the listing of intent
items, and the checks. All three start in `spawn()` in `src/host/compile.c`. The confinement goes
there, after the resource limits and before `execve`.

**The mechanism is Landlock and a seccomp filter together.** The probe shows that the pair
refuses every attempt on the development host and on a GitHub-hosted `ubuntu-24.04` runner,
with no privilege and no setup. Landlock alone leaves UDP and Unix sockets with a path. A
seccomp filter alone leaves every file. Namespaces cannot be set up on the runner.

What a child may reach:

| Access | What |
| --- | --- |
| Read and execute | `/usr`, `/lib`, `/lib64`, `/etc/ld.so.cache` and the `mo` executable |
| Read | The `lib/` folder, and the output of the children that ran before it |
| Read and write | Its own output folder, and `/dev/null` |
| Nothing | Every other file, every socket, and a signal to or a trace of any other process |

A sanitizer build also reads files under `/proc/self`. The confinement milestone adds what that
build needs and no more.

The seccomp filter is a list of refused system calls: sockets, signals to another process,
tracing, reading another process's memory, `io_uring`, namespaces and mounting. A list of
allowed calls would be stricter. It must name every call the C library and a sanitizer make,
and Landlock already covers files, so the refused list is the proposal.

**Each child gets its own output folder.** Today all three share one scratch folder, and the
parent reads `image.bc` and `items.moil` only after the check child has exited. A check child
taken over through a VM defect could replace both and pass the door. With a folder for each
child, a later child can read what an earlier one wrote and cannot change it.

**On a host that cannot confine, `mo serve` refuses to accept an upgrade.** macOS has neither
mechanism, and neither do old kernels and some containers. The server says so when it starts,
and an upgrade gets a diagnostic. One flag, `--unconfined`, lets a development host accept
upgrades. The server then says at start, and in the result of every upgrade, that the children
are not confined.

**The control is a build without the confinement.** A second build of `compile.c` has the
confinement call renamed away at compile time, which is how the Makefile builds its fault
tests. The check runs a probe program as the child through the same `spawn()`. It must pass on
the normal build and fail on the control build.

What confinement does not do:

- A child can still forge its own output. A check child taken over can report a pass.
- It does not confine `mo serve` itself, or a program under `mo run` or `mo test`.
- It does not protect against a defect in the kernel.

## What the measurement found

The bar was committed before the measurement
([bar.md](../../learnability/phase7/preflight/http-in-mo/bar.md)): one `mo run` that fetches
about 1 MiB of JSON over a connection, parses HTTP in Mo and calls `Json.parse`, in at most
2.0 seconds and 256 MiB of heap, on the debug build.

| Figure | Bar | `Content-Length` | Chunked | Result |
| --- | --- | --- | --- | --- |
| Wall time | 2.0 s | 5.61 s | 5.83 s | **Missed** |
| Heap peak | 256 MiB | 37.0 MiB | 37.0 MiB | Met |

The miss is not caused by the choice the bar was written to test. `Json.parse` alone, on the
same body read from a file, takes 5.36 seconds and 109,677,820 instructions. The whole fetch
takes 109,685,165. HTTP parsed in Mo is the difference: 7,345 instructions, or 95,999 when the
response is chunked. That is under 0.1%.

`Json.parse` turns the text into a `List(Char)` and reads one cell at a time, at about 105
instructions for each byte. It costs the same for a file as for a response, and it costs that
today.

A 100 KiB response takes 0.63 seconds. An optimized build takes 2.16 seconds for 1 MiB, which
also misses.

The bar file says a miss stops the design step until Robert chooses. The choices:

1. **Approve the connection level, and record the parser's speed as a known cost.** The spike
   runs with it. JSON parsing becomes the first measured workload of the phase about speed,
   which was waiting for a real program to measure. This is the recommendation: the level costs
   under 0.1%, the usual response is inside a second, and a parser tuned now is tuned without
   the programs that would show what matters.
2. **Rework the parser in this phase**, on primitives that scan `Text` without a list. How much
   that gains is not measured.
3. **Move JSON parsing into C**, with a fuzz target. It would meet the bar. It is new C that
   reads network bytes where Mo can do the job, which the phase's safety rule is against.

## Where a capability is written down

`src/base/hostops.def` gives each operation one row, which the checker, the code generator, the
verifier, the VM and the host all read. A new capability type also touches the places below,
which are kept by hand. The two capabilities edit the same places, so SQLite merges after HTTP.

The new types take the numbers after `Simulation`: bytecode holds these numbers, so none that
exists may change. Host capabilities are then no longer the unbroken range from `Args` to
`Random`. Every array sized `MO_CAP_RANDOM + 1` and every loop up to `MO_CAP_RANDOM` gets a new
bound, and still skips `Simulation`.

| Module | Places |
| --- | --- |
| `base` | The count in `capabilities.h` and the names in `capabilities.c`; named limits in `limits.h` |
| `heap` | The `mo_capability` enum and the range check in `types.c` |
| `check` | Names always in scope (`modules.c`); built-in types and their arity (`signatures.c`); the rule for `main`'s parameters (`program.c`), which must keep `Connection` and `Sqlite` out as it keeps `Simulation` out |
| `lower` | Name to Core tag (`types.c`); the table from `Files.read_text` to a Core operation (`applications.c`) |
| `core` | Capability tags and the operations outside the list (`core.h`); `MO_CORE_IO_OPS` (`ops.def`); names, the operation codec and the capability-state record (`text_schema.def`); the typing of each operation (`check.c`); the Core text version |
| `codegen` | Core tag to capability number and the signature parser (`types.c`); the rows that must stay in `MO_CORE_IO_OPS` order (`operations.c`) |
| `bytecode` | Range checks in `read.c`, `indexed.c`, `shorthand-type.c` and `disassemble.c` |
| `vm` | The per-capability arrays (`internal.h`); what `main` is handed (`process.c`); `mo_call_main`, which takes the arguments, the environment and the folder by hand (`vm.h`) |
| `host` | The count of private types and the fake-state union (`host.h`); the operation table, the fake constructors, the real capabilities and the restore, snapshot and drop of fakes (`host.c`); capability slots and private types (`types.c`); a counter and a release function for each handle that owns a descriptor, as `real-files.c` has for folders |
| `tools` | The fake factories that promise generation uses (`fakes.h`, `fakes.c`); the list of types in `promises.c`, with the count beside it; what `mo run` prepares (`run.c`) |
| Tests | `test_tables.c` (types, the rules for a row, the operation list), `test_heap.c`, `test_opcodes.c`, `test_fakes.c`, `test_types.c`, `test_bytecode_io.c`, `test_host.c`, `heap_fixture.h`, `host_fixture.h`, `vm_run.h` |
| Fuzz and CBMC | `fuzz_host.c`, `fuzz_bytecode_io.c`, `fuzz_host_real.c`; `cbmc/verify.c` |
| Lean | The capability and operation types, their names, signatures and the spec-only set (`Syntax.lean`); evaluation (`Prim.lean`); capability state (`Value.lean`); the text form (`Text/`) |
| `ci` | `check_host.py`, `check_core_check.py`, `core_library_map.py`, `check_lower_source.py`, `library_map.sh` |
| Documents | `bytecode.md` sections 2.1 and 7 and its count of capability names; the reference, sections 10.3, 11, 12.4, 13.4 and 14; `library.md`; `formal/TEXT.md` and `TRANSLATION.md`; `docs/agent-guide.md` |

Two costs are not on the plan's list:

- **The host can build values of six types** (`Int`, `Time`, `Text`, a list, a set and a map of
  texts). Results that hold `Bytes`, the fake's maps and SQLite's rows each need more.
- **`ci/check_host.py` has no way to skip a row.** It fails when a row has no modelled
  counterpart in Lean. See the next section.

## Lean and Mo Core

Lean models neither capability, as it does not model `Simulation`. Each operation is still a Mo
Core operation with a type in Lean, so Core programs that use it are checked. Evaluating one
stops Lean with "not modeled by mocore", and a run that reaches the network or SQLite, real or
fake, drops out of the comparison between the VM and Lean.

The operations stay ordinary rows in `hostops.def`, and `ci/check_host.py` gains a named set of
rows that stop outside the model. `Simulation` took the other road, a separate family of rows
with its own lowering, and that plumbing is larger. The Core text version goes up, as it did
for `Simulation`.

The client's parsing functions take plain values and stay inside the comparison. So do
`Bytes.slice`, `Bytes.join` and the JSON lookup.

## Names this takes

`Network`, `Connection`, `Sqlite`, `NetworkError` and `SqliteError` become type names that are
always in scope, as `Files` and `FileError` are. `Http` becomes a library module's name. A
program that declares any of the six stops compiling. No program in the repository does, apart
from `runtime/http.mo`.

The cases of the new enums are not always in scope. `FileError`'s are, which is why no program
may have a case called `NotFound` or `Failed`. Doing the same here would take `Refused`,
`Closed`, `TimedOut`, `Null`, `String` and more from every program. A program writes the
module's name in front, `Network.Refused(host, port)` and `Sqlite.Integer(7)`, as it does for
an enum in a library module. No built-in type has cases that work this way today. The HTTP
milestone builds that, or declares these types in `lib/` and teaches the host to build their
values.

## The threat model

`docs/design/sandbox-properties.md` and `docs/design/intent.md` change when each part is built,
and not before:

- **Confinement** changes gap 4 (no operating-system confinement) and gap 7 (the upgrade
  credential reaches the compiler). A memory-safety defect in a child is then contained to what
  the table above allows. Forging its own output stays possible, and the text keeps saying so.
- **The network capability** changes property P7. A program reaches the network only with a
  `Network` that `main` lists. A program under `mo run` is not confined: it runs as its user, as
  any script does.
- **A service still holds no capability.** It cannot call out, and it cannot open a database.

## What this leaves out

- Services that call out, and anything like a web framework.
- Postgres.
- Redirects followed for the caller, retries, cookies, proxies, compression and connection
  reuse.
- A request body from raw `Bytes`, and helpers for `PUT`, `PATCH` and `DELETE`.
  `Http.request` covers the methods.
- A fake that gives a different reply to the same request the second time.
- A database opened read-only, the number of rows a statement changed, and columns read by
  name.
- Lookup that gives a decimal or a float by path.
- Confinement of `mo serve` itself and of `mo run`.
- A faster JSON parser, unless Robert chooses otherwise.

## Decisions for Robert

Rows 2 to 5, 12 and 13 are calls the plan made without him. Rows 7, 8, 10 and 11 are questions
the plan left open. The rest came up in this step. Each needs his yes or a change.

| # | Decision | Recommendation | The other choice |
| --- | --- | --- | --- |
| 1 | The 1 MiB fetch missed the wall-time bar, because of `Json.parse`. | Approve the connection level, record the parser's speed, and make it the first workload of the phase about speed. | Rework the parser now, or move it into C. |
| 2 | The level of the network capability. | A connection, with HTTP in Mo. The level itself costs under 0.1%. | An HTTP request as the operation, with HTTP parsed in C. |
| 3 | The fake's level, and how a test builds it. | The same level as the capability. `Http.replies` builds it from requests and responses, and the match is exact. | A fake that matches on method and URL only, which needs HTTP parsing outside Mo. |
| 4 | Lean's model. | Neither capability is modelled. Runs that reach one leave the comparison. | Model the fakes in Lean, as `Files` is. |
| 5 | What a program's database may do. | What SQLite allows by default, except opening any other file. | The service store's stricter rules. |
| 6 | SQL can read the clock and draw random numbers with no `Clock` or `Random`. | Record it in the threat model, and decide in the SQLite milestone whether to refuse those functions. | Refuse them from the start. |
| 7 | The authority of the SQLite capability. | From `Files`: `files.sqlite(path)`, bounded by that folder. | A `Sqlite` capability on `main` with its own rule about paths. |
| 8 | The client's module name. | `Http`, with the runtime's module renamed. | `HttpClient`, with the runtime left alone. |
| 9 | `Bytes.slice` and `Bytes.join` as new primitives. | Add both. | |
| 10 | The confinement mechanism. | Landlock and a seccomp filter together. | |
| 11 | A host that cannot confine. | `mo serve` refuses upgrades, and `--unconfined` overrides it out loud. | Run unconfined and say so. |
| 12 | Where the mo-lang skill lives, and when the guide changes. | The skill lives in this repository. Each capability's pull request updates `docs/agent-guide.md`. | |
| 13 | The form of the agent evaluation. | Agents write programs that hidden tests grade, under Phase 5's rules for the gate, the freeze and publication. | |
| 14 | The new text of section 15 of the reference. | As this pull request writes it. | |
| 15 | The release that "Fast" carries, now that it is Phase 8. | None named yet. | v0.5. |
