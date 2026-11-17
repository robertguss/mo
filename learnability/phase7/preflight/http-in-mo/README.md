# HTTP written in Mo: what a 1 MiB response costs

A throwaway prototype, built on commit 2b6335c3, that measures the bet in Phase 7's plan: the
network capability works at the level of a connection, and HTTP is parsed in Mo.

[bar.md](bar.md) holds the bar. It was committed (006733fe) before any number here was taken.

Verdict: **the wall-time bar is missed, and the heap bar is met.** The miss is not in the HTTP
code. `Json.parse` in `lib/json.mo`, which exists today, takes all but 0.1% of the work. HTTP
parsed in Mo adds 7,345 instructions to a 1 MiB response with `Content-Length` and 95,999 to a
chunked one, out of 109.7 million.

## Results beside the bar

Median of five runs of the whole `mo run`, debug build, 1 MiB body (1,048,867 bytes):

| Figure | Bar | `Content-Length` | Chunked | Result |
| --- | --- | --- | --- | --- |
| Wall time | 2.0 s | 5.61 s | 5.83 s | **Missed** |
| Heap peak | 256 MiB | 37.0 MiB | 37.0 MiB | Met |

Where the time goes, on the same body and the same build:

| Run | Wall time | Instructions | Heap peak |
| --- | --- | --- | --- |
| Fetch over the connection, parse HTTP in Mo, `Json.parse` | 5.61 s | 109,685,165 | 37.0 MiB |
| The same, chunked | 5.83 s | 109,773,819 | 37.0 MiB |
| `Json.parse` alone, body read from a file | 5.36 s | 109,677,820 | 37.0 MiB |
| A program that prints one line | 0.08 s | | |

`Json.parse` runs about 105 instructions for each byte of the body. It turns the text into a
`List(Char)` and reads one cell at a time. The debug build runs about 20 million instructions a
second.

The cases the bar file lists with no bar:

| Case | Debug build | Optimized build |
| --- | --- | --- |
| 1 MiB, `Content-Length` | 5.61 s | 2.16 s |
| 1 MiB, chunked | 5.83 s | 2.22 s |
| 1 MiB, `Json.parse` alone | 5.36 s | 2.06 s |
| 100 KiB, `Content-Length` | 0.63 s | 0.29 s |
| 100 KiB, chunked | 0.61 s | 0.30 s |
| 100 KiB, `Json.parse` alone | 0.60 s | 0.31 s |
| 1 MiB, `Content-Length`, with only the `Bytes` operations Mo has today | 14.04 s, 60 MiB heap | 4.75 s |
| 1 MiB, chunked, with only the `Bytes` operations Mo has today | 14.80 s, 84 MiB heap | 5.44 s |

Every row of every run, with its exit status, is in [out/runs.txt](out/runs.txt), and the
medians with their ranges are in [out/summary.md](out/summary.md). All 100 runs exit 0 and print
the same line for the same body.

## What the numbers say

- **The level of the capability costs almost nothing.** With `Bytes.slice` and `Bytes.join`,
  receiving the response, finding the end of the headers, reading the status line and the
  headers, and removing the chunk framing is under 0.1% of the work.
- **The two proposed operations are needed.** Without them the same program copies each byte into
  a list and decodes UTF-8 in Mo. That takes 2.5 times the instructions and up to 2.3 times the
  heap.
- **The JSON parser is the cost, and it is the same cost for a file.** A program that reads a
  1 MiB JSON file today waits as long.
- **The usual case is inside a second.** A 100 KiB response takes 0.63 s on the debug build.
- **An optimized build does not meet the bar either.** It takes 2.16 s and 2.22 s.

## Files

| File | What it is |
| --- | --- |
| `bar.md` | The bar, written first. |
| `prototype.patch` | The change to `src/` at 2b6335c3. It adds `Bytes.slice(from, to)` and `Bytes.join(parts)` as primitives. It makes a `Files` path of the form `tcp:PORT` stand for one loopback connection, so the prototype needs no new capability. It prints the instruction count and the heap peak when `MO_PROTOTYPE_STATS` is set. |
| `proposed/fetch.mo` | The fetch with the two proposed operations. |
| `today/fetch.mo` | The same fetch with `size`, `get` and `decode_utf8` only. |
| `parse-only/parse.mo` | Reads the body from a file and calls `Json.parse`. |
| `server.py` | The local server. `GET /fixed/SIZE` answers with `Content-Length`, and `GET /chunked/SIZE` with chunks of 8,192 bytes. The body is an array of objects with text, number, boolean and null fields, some of the text outside ASCII, and is the same on every run. |
| `run.sh` | Exports 2b6335c3 from git into a temporary folder, applies the patch, builds debug and optimized, and runs every case five times. |

Run it with `bash learnability/phase7/preflight/http-in-mo/run.sh`. It takes about ten minutes,
and uses port 18123 unless `PORT` is set.

## What this does not show

- The connection is a hack inside the `Files` capability. It has no timeout, no name lookup and
  no error cases. The real capability is new code in `src/host/`.
- The client is 101 lines that handle one request. It reads the response until the server closes
  the connection, and it does not check `Content-Length` against what arrived.
- No TLS. Encryption sits under the connection and is C, so it adds no interpreter time.
- One host and one body shape. A body of long strings would cost fewer instructions for each
  byte, and a body of short numbers more.
- Nothing here says how fast a parser that reads `Text` or `Bytes` through primitives would be.
  That was not built.
