# Mo talks to the outside world

Status: proposed on 2026-11-17 as Phase 7's design. Robert has not approved it, and nothing in
it is built. Every choice below that is his to make is in
[Decisions for Robert](#decisions-for-robert), each with a recommendation. The evidence, with its
commands and raw outputs, is in [the preflight](../../learnability/phase7/preflight/README.md).

A Mo program gets two new capabilities: the network and a SQLite database. An HTTP client ships
in `lib/`, written in Mo on top of the network capability. A program can look through a JSON
response without declaring types first. The children that `mo serve` starts to compile and check
pushed source run under operating-system confinement.

The first measurement missed the bar that was written down before it, because of `Json.parse`.
Robert asked for a faster parser in this step. The parser in `lib/json.mo` was reworked in Mo,
and the second measurement meets the bar. It is the first decision in the list.

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

fn main(args: Args, env: Env, console: Console, network: Network, sqlite: Sqlite) : Result(
  Unit,
  Halt
)
  repo = try args.list.first.or_fail(Usage)
  database = try sqlite.open("issues.db")
  saved = try sync(network, database, repo, env.get("API_TOKEN") or "")
  console.print("saved #{saved} open issues")
  found = try database.query("select number, title from issues order by number", [])
  for row in found.rows
    console.print("#{row}")
  end
  Ok(Unit)
end

## Fetches the open issues of `repo` and saves each one. Gives the number saved.
fn sync(network: Network, database: Sqlite.Database, repo: Text, token: Text) : Result(Int, Halt)
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
- The program opens a SQLite database through `Sqlite`, a capability `main` lists. Statements
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
  assert Sqlite.fake().open("issues.db") is Ok(database)
  assert sync(network, database, "mo/mo", "t0ken") == Ok(2)
  assert database.query("select title from issues order by number", []) is Ok(found)
  assert found.rows == [[Sqlite.String("Crash on empty input")], [Sqlite.String("Slow start")]]
end

test "a request the fake holds no reply for is an error"
  network = Network.fake(Http.replies([]))
  assert Sqlite.fake().open("issues.db") is Ok(database)
  assert sync(network, database, "mo/mo", "t0ken") is Error(Call(_))
end
```

What it shows:

- The fake network answers from the responses the test supplies. The test reads as a request
  and a response and never as raw bytes.
- A request the fake holds no reply for is an error the program receives, and no real
  connection is made.
- The database in the test is in memory. It comes from a fake `Sqlite`, which only tests,
  examples and promises can make.

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
- When the fake holds a reply for a request with the same first line, that reason must also show
  what differs. A test that fails over one header says which header.
- With an entry, `receive` hands out the reply, at most `most` bytes at a time, and then the
  end.
- A fake never opens a socket and never reads a clock. `within` is ignored.
- `connect_secure`, when the HTTPS milestone adds it, answers from the same map. A fake does no
  TLS, so a test of an `https` URL states the request as plain bytes.

A test does not write these bytes. `Http.replies` builds the map from requests and responses
(below). The client writes a request the same way every time, in whatever order the program
added its headers, so the match is exact: a test states the whole request, headers included, and
a program that sends anything else gets an error.

Exact matching can make tests brittle: a program that adds one header changes every test that
states that request. Robert's spike shows whether that is too brittle in real use. If it is,
this decision is taken again.

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

Without them the same fetch copies each byte into a list. It takes 6.7 to 7.9 times the
instructions and up to 2.0 times the heap.

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
  BadHeader(name: Text)
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
- It writes its own headers first, in a fixed order. The program's headers follow, sorted by
  name without regard to case. Two headers with the same name keep the order the program gave
  them. So the order of `header` calls never changes what is sent.
- It reads a response with `Content-Length`, with chunked transfer encoding, or ended by the
  close of the connection.
- A status of 404 or 500 is a response. It is not an error.
- It follows no redirect, retries nothing, keeps no cookie, uses no proxy, reuses no
  connection, and asks for no compression. Robert's spike shows which of these he misses.
- A URL is `http://` or `https://`, a host, an optional port, a path and a query. Anything else
  is `BadUrl`. Until the HTTPS milestone is built, an `https` URL is an error that says so.
- A header whose name or value holds a line break is `BadHeader`, and nothing is sent. A token
  read from a file with its last newline would otherwise add a line to the request.
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

Two types are new, as for the network. `Sqlite` is the capability `main` lists.
`Sqlite.Database` is one open database. A program gets a `Sqlite.Database` only from a `Sqlite`,
and `main` cannot list one.

| Operation | Signature | What it does |
| --- | --- | --- |
| `sqlite.open(path)` | `fn(Sqlite, Text) : Result(Sqlite.Database, SqliteError)` | Opens the database file at `path`, and creates it when it is absent. |
| `Sqlite.fake()` | `fn() : Sqlite` | A `Sqlite` that keeps its databases in memory. Only tests, examples and promises may call it. |
| `database.run(statement, values)` | `fn(Sqlite.Database, Text, List(Sqlite.Value)) : Result(Unit, SqliteError)` | Runs one statement and gives no rows. |
| `database.query(statement, values)` | `fn(Sqlite.Database, Text, List(Sqlite.Value)) : Result(Sqlite.Rows, SqliteError)` | Runs one statement and gives its rows. |

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

`Sqlite` is a capability of its own. `mo run` hands the program a `Sqlite` bounded by the
folder the program was started in, which is the folder that its `Files` reaches. There is no
second rule about paths: `path` is checked as every `Files` path is, by the same code, and one
that leaves the folder gives `Error(OutsideFolder(path))`. SQLite's journal files sit beside
the database, in the same folder.

Robert chose this over `files.sqlite(path)`, which the first draft proposed:

- `main` shows that a program keeps a database.
- A program can hold a database without holding every file in the folder.
- A database that is not a file, such as Postgres, could never come from `Files`.

A program that lists `Sqlite` and no `Files` reaches every SQLite database in the folder, and
can make new ones. It reads no other file, because SQLite refuses a file that is not a
database. An empty file counts as a new database.

Each database that Mo supports later keeps this shape where it can: open, `run`, `query`,
values and rows.

`Files` opens a file beneath its folder and follows no symbolic link. SQLite opens a database
by its path, and its journals by paths it builds. The SQLite milestone must show that a
symbolic link in the folder that points outside it gives `OutsideFolder` for the database and
for its journals, as it does for a file.

A `Sqlite.Database` holds the one file it opened, and no statement opens another:

- `ATTACH` and `VACUUM INTO` are refused with `NotPermitted`.
- Extension loading is already left out of the build.
- Temporary storage stays in memory.

Without the second half of that, a test in pushed source could make a database in memory and
use it to read or write the host's files from inside the check child.

Everything else SQLite does by default is allowed: triggers, views, transactions written as
statements. The service store's own rules stay the store's.

A fake keeps its databases in memory and touches no file. Two opens of one path on one fake
give the same database, as two handles on one `Files.in_memory` share their files. Two fakes
share nothing. `OutsideFolder` is given for the same paths as on the real capability.

One hole is known. SQL can read the clock (`datetime('now')`) and draw random numbers
(`random()`), and the program needs no `Clock` or `Random` to do it. It is on the list of
decisions.

### How it is built

The capability uses the SQLite already linked into every `mo`, and its code sits beside the
service store in `src/host/`. It shares the store's allocator setup, which is made once for the
process under one memory budget. A `Sqlite.Database` owns its database handle and closes it
when the program drops the last reference.

A fake must keep a database between two opens of its path. The rule for a fake's state in
`docs/design/bytecode.md` allows numbers, texts, bytes and collections of them, and an open
database is none of those. The SQLite milestone chooses between a change to that rule and a
fake that stores each database as bytes.

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
| Read | The `lib/` folder, and the inputs the parent wrote for it: the pushed bundle, the running bundle and the upgrade's context |
| Read and write | Its own output folder, and `/dev/null` |
| Nothing | Every other file, every socket, and a signal to or a trace of any other process |

The inputs sit in a folder no child can write. Today they sit in the one scratch folder. If
they stayed where a child writes, the compile child could replace the bundle that the check
child then reads.

A sanitizer build also reads files under `/proc/self`. The confinement milestone adds what that
build needs and no more.

The seccomp filter is a list of refused system calls: sockets, signals to another process
(`kill`, `tkill`, `tgkill` at another process, `rt_sigqueueinfo`, `rt_tgsigqueueinfo` and
`pidfd_send_signal`), tracing, reading another process's memory, `io_uring`, mounting, and new
namespaces by `unshare`, `setns`, or `clone` and `clone3` with a namespace flag. The probe's
filter has all of these except the two `clone` rules. Landlock scopes signals only from kernel
6.12, so on an older kernel the filter is the one thing that stops them. A list of
allowed calls would be stricter. It must name every call the C library and a sanitizer make,
and Landlock already covers files, so the refused list is the proposal.

**Each child gets its own output folder.** Today all three share one scratch folder, and the
parent reads `image.bc` and `items.moil` only after the check child has exited. A check child
taken over through a VM defect could replace both and pass the door. With a folder for each
child, no child can change what another wrote. No child reads another's output today: the
listing and the checks read the pushed bundle, as the compile does.

**On a host that cannot confine, `mo serve` refuses to accept an upgrade.** macOS has neither
mechanism, and neither do old kernels and some containers. The server says so when it starts,
and an upgrade gets a diagnostic. One flag, `--unconfined`, lets a development host accept
upgrades. The server then says at start, and in the result of every upgrade, that the children
are not confined.

**The control is a build without the confinement.** A second build of `compile.c` has the
confinement call renamed away at compile time, which is how the Makefile builds its fault
tests. The check runs a probe program as the child through the same `spawn()`. It must pass on
the normal build and fail on the control build. Its attempts are the preflight probe's, the
queued signal and the parent's inputs among them.

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
| Wall time, first measurement | 2.0 s | 5.61 s | 5.83 s | **Missed** |
| Wall time, with the faster parser | 2.0 s | 1.88 s | 1.84 s | Met |
| Heap peak, with the faster parser | 256 MiB | 41.7 MiB | 41.7 MiB | Met |

The miss was not caused by the choice the bar was written to test. In the first measurement
`Json.parse` alone, on the same body read from a file, took 5.36 seconds and 109,677,820
instructions, and the whole fetch took 109,685,165. `Json.parse` turned the text into a
`List(Char)` and read one cell at a time, at about 105 instructions for each byte.

The bar file says a miss stops the design step until Robert chooses. He asked for a faster
parser in this step. `Json.parse` now cuts the text at its quotes with `text.split`, and reads
a string with no escape and no control character as one piece. Every other string takes the
walk it took before, so every result and every error is the same. The parser is still Mo, with
no new primitive. It takes 28,842,519 instructions for the same body, about 28 for each byte.

HTTP parsed in Mo adds 7,392 instructions to that, or about 96,000 when the response is
chunked. That is under 0.4% of the instructions.

The margin is small: the slower of the two medians is 6% under the bar. A 100 KiB response
takes 0.27 seconds. An optimized build takes 0.80 seconds for 1 MiB.

The gain is for strings with no escape. A document made mostly of numbers, or of strings with
escapes, parses about as slowly as before: 8.3 and 9.0 seconds for documents of 1.2 and 1.7 MB
([json-parse/](../../learnability/phase7/preflight/json-parse/README.md)). A document printed
with an indent takes 2.9 seconds for 1.4 MB. Those wait for the phase about speed. A parser in C would be new C that reads network bytes where Mo can do the
job, which the phase's safety rule is against.

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
| `check` | Names always in scope (`modules.c`); built-in types and their arity (`signatures.c`); the rule for `main`'s parameters (`program.c`), which must keep `Connection` and `Sqlite.Database` out as it keeps `Simulation` out |
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

**Lean models the fake network.** Robert chose this over a model of neither capability. The
fake is a map from the bytes sent to the bytes received, with three operations, and Lean
already models fakes with more state. The model gives three things:

- A program that calls an API stays in the comparison between the VM and Lean for its whole
  run. Without the model a run leaves the comparison at its first `connect`, so
  `request.send` would never be compared from start to end.
- `ci/check_host.py` runs random sequences of `connect`, `send` and `receive` on the C fake
  and on the model, as it does for the other fakes.
- The fake gets an exact definition. The model must say what each odd case gives, such as a
  `receive` before any `send`, and a `send` after the reply has started.

The real network is not modelled. A run that reaches it stops Lean with "not modeled by
mocore", as a run that reaches `Simulation` does.

The model is a pull request of its own, after the one that adds the capability. Until it
merges, the fake's operations also stop outside the model.

**Lean does not model SQLite**, real or fake. A model would need an SQL engine in Lean. Each
SQLite operation is still a Mo Core operation with a type in Lean, so Core programs that use it
are checked, and a run that reaches one drops out of the comparison.

All of these operations are ordinary rows in `hostops.def`, and `ci/check_host.py` gains a
named set of rows that stop outside the model. The pull request for the model takes the fake
network's rows out of that set. `Simulation` took the other road, a separate family of rows
with its own lowering, and that plumbing is larger. The Core text version goes up, as it did
for `Simulation`.

The client's parsing functions take plain values and are compared in every case. So are
`Bytes.slice`, `Bytes.join` and the JSON lookup.

## Names this takes

`Network`, `Connection`, `Sqlite`, `NetworkError` and `SqliteError` become type names that are
always in scope, as `Files` and `FileError` are. `Http` becomes a library module's name. A
program that declares any of the six stops compiling. No program in the repository does, apart
from `runtime/http.mo`. `Sqlite.Database`, `Sqlite.Value` and `Sqlite.Rows` are written with the
module's name in front, so they take no name from a program.

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
- **The SQLite capability** follows the same rule. A program opens a database only with a
  `Sqlite` that `main` lists.
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
- A `Sqlite` narrowed to a folder inside the one the program was started in.
- Lookup that gives a decimal or a float by path.
- Confinement of `mo serve` itself and of `mo run`.
- A faster parse of a document made mostly of numbers or of strings with escapes.

## Decisions for Robert

Rows 2 to 5, 12 and 13 are calls the plan made without him. Rows 7, 8, 10 and 11 are questions
the plan left open. The rest came up in this step. Each needs his yes or a change.

Robert's answers so far, on 18 November 2026: row 2, yes. Row 3, yes, with headers written
sorted by name and a mismatch reason that shows what differs. Row 7, changed: a `Sqlite`
capability of its own, in place of `files.sqlite(path)`. Rows 5, 6, 10
and 11, yes. Row 4, changed: Lean models the fake network.

| # | Decision | Recommendation | The other choice |
| --- | --- | --- | --- |
| 1 | The 1 MiB fetch first missed the wall-time bar, because of `Json.parse`. Robert asked for a faster parser in this step. | Accept the reworked parser in `lib/json.mo`, which meets the bar in Mo. Documents of numbers or of strings with escapes stay slow, and become the first workload of the phase about speed. | A primitive in C that scans text, for those documents, in this phase. |
| 2 | The level of the network capability. | A connection, with HTTP in Mo. The level itself costs under 0.4% of the instructions. | An HTTP request as the operation, with HTTP parsed in C. |
| 3 | The fake's level, and how a test builds it. | The same level as the capability. `Http.replies` builds it from requests and responses, and the match is exact. | A fake that matches on method and URL only, which needs HTTP parsing outside Mo. |
| 4 | Lean's model. | Lean models the fake network, in a pull request of its own after the capability lands. It does not model SQLite. | Neither capability is modelled, and a run that reaches one leaves the comparison. |
| 5 | What a program's database may do. | What SQLite allows by default, except opening any other file. | The service store's stricter rules. |
| 6 | SQL can read the clock and draw random numbers with no `Clock` or `Random`. | Record it in the threat model, and decide in the SQLite milestone whether to refuse those functions. | Refuse them from the start. |
| 7 | The authority of the SQLite capability. | A `Sqlite` capability on `main`: `sqlite.open(path)`, bounded by the folder the program was started in, with the path check that `Files` uses. | From `Files`: `files.sqlite(path)`. |
| 8 | The client's module name. | `Http`, with the runtime's module renamed. | `HttpClient`, with the runtime left alone. |
| 9 | `Bytes.slice` and `Bytes.join` as new primitives. | Add both. | |
| 10 | The confinement mechanism. | Landlock and a seccomp filter together. | |
| 11 | A host that cannot confine. | `mo serve` refuses upgrades, and `--unconfined` overrides it out loud. | Run unconfined and say so. |
| 12 | Where the mo-lang skill lives, and when the guide changes. | The skill lives in this repository. Each capability's pull request updates `docs/agent-guide.md`. | |
| 13 | The form of the agent evaluation. | Agents write programs that hidden tests grade, under Phase 5's rules for the gate, the freeze and publication. | |
| 14 | The new text of section 15 of the reference. | As this pull request writes it. | |
| 15 | The release that "Fast" carries, now that it is Phase 8. | None named yet. | v0.5. |
