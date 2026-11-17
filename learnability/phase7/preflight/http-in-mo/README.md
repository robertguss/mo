# HTTP written in Mo: what a 1 MiB response costs

A throwaway prototype, built on commit 2b6335c3, that measures the bet in Phase 7's plan: the
network capability works at the level of a connection, and HTTP is parsed in Mo.

[bar.md](bar.md) holds the bar. It was committed (006733fe) before any number here was taken.

Verdict: **both bars are met, on the second measurement.** The first measurement missed the
wall-time bar, and the miss was not in the HTTP code: `Json.parse` in `lib/json.mo` took all but
0.1% of the work. The parser was then reworked in Mo (4968cc2d,
[json-parse/](../json-parse/README.md)), and the same runs were taken again on that commit.
HTTP parsed in Mo adds 7,392 instructions to a 1 MiB response with `Content-Length` and about
97,000 to a chunked one, out of 28.3 million.

## Results beside the bar

Median of five runs of the whole `mo run`, debug build, 1 MiB body (1,048,867 bytes):

| Figure | Bar | `Content-Length` | Chunked | Result |
| --- | --- | --- | --- | --- |
| Wall time, first measurement (2b6335c3) | 2.0 s | 5.61 s | 5.83 s | **Missed** |
| Wall time, faster parser (4968cc2d) | 2.0 s | 1.74 s | 1.78 s | Met |
| Heap peak, first measurement | 256 MiB | 37.0 MiB | 37.0 MiB | Met |
| Heap peak, faster parser | 256 MiB | 41.7 MiB | 41.7 MiB | Met |

Where the time goes, on the same body and the same build:

| Run | First measurement | Faster parser |
| --- | --- | --- |
| Fetch over the connection, parse HTTP in Mo, `Json.parse` | 5.61 s, 109,685,165 instructions | 1.74 s, 28,257,983 instructions |
| The same, chunked | 5.83 s, 109,773,819 | 1.78 s, 28,347,913 |
| `Json.parse` alone, body read from a file | 5.36 s, 109,677,820 | 1.70 s, 28,250,591 |
| A program that prints one line | 0.08 s | |

In the first measurement `Json.parse` ran about 105 instructions for each byte of the body. It
turned the text into a `List(Char)` and read one cell at a time. It now runs about 27. The debug
build runs about 20 million instructions a second.

The cases the bar file lists with no bar, with the faster parser:

| Case | Debug build | Optimized build |
| --- | --- | --- |
| 1 MiB, `Content-Length` | 1.74 s | 0.82 s |
| 1 MiB, chunked | 1.78 s | 0.80 s |
| 1 MiB, `Json.parse` alone | 1.70 s | 0.75 s |
| 100 KiB, `Content-Length` | 0.27 s | 0.17 s |
| 100 KiB, chunked | 0.28 s | 0.16 s |
| 100 KiB, `Json.parse` alone | 0.25 s | 0.17 s |
| 1 MiB, `Content-Length`, with only the `Bytes` operations Mo has today | 8.88 s, 60 MiB heap | 3.49 s |
| 1 MiB, chunked, with only the `Bytes` operations Mo has today | 10.64 s, 84 MiB heap | 4.25 s |

Every row of every run, with its exit status, is in [out/runs.txt](out/runs.txt) for the first
measurement and [out-faster-parser/runs.txt](out-faster-parser/runs.txt) for the second. The
medians with their ranges are in the `summary.md` beside each. In each measurement all 100 runs
exit 0 and print the same line for the same body.

The two fetch programs gained a usage line after the first measurement, so that `mo run` with no
arguments ends: the format check runs every tracked program that way. The line costs 47
instructions, and the second measurement includes it.

## What the numbers say

- **The level of the capability costs almost nothing.** With `Bytes.slice` and `Bytes.join`,
  receiving the response, finding the end of the headers, reading the status line and the
  headers, and removing the chunk framing is under 0.4% of the work.
- **The two proposed operations are needed.** Without them the same program copies each byte into
  a list and decodes UTF-8 in Mo. That takes 6.8 to 8.1 times the instructions and up to 2.0 times
  the heap.
- **The JSON parser was the cost, and it was the same cost for a file.** A program that reads a
  1 MiB JSON file gains as much as a fetch does.
- **The usual case is well inside a second.** A 100 KiB response takes 0.27 s on the debug build.
- **The faster parser holds 4.7 MiB more at its peak.** It keeps the pieces between the quotes
  beside the list of characters.

## Files

| File | What it is |
| --- | --- |
| `bar.md` | The bar, written first. |
| `prototype.patch` | The change to `src/` at 2b6335c3. It adds `Bytes.slice(from, to)` and `Bytes.join(parts)` as primitives. It makes a `Files` path of the form `tcp:PORT` stand for one loopback connection, so the prototype needs no new capability. It prints the instruction count and the heap peak when `MO_PROTOTYPE_STATS` is set. |
| `proposed/fetch.mo` | The fetch with the two proposed operations. |
| `today/fetch.mo` | The same fetch with `size`, `get` and `decode_utf8` only. |
| `parse-only/parse.mo` | Reads the body from a file and calls `Json.parse`. |
| `server.py` | The local server. `GET /fixed/SIZE` answers with `Content-Length`, and `GET /chunked/SIZE` with chunks of 8,192 bytes. The body is an array of objects with text, number, boolean and null fields, some of the text outside ASCII, and is the same on every run. |
| `run.sh` | Exports a commit from git into a temporary folder (2b6335c3, or `BASE`), applies the patch, builds debug and optimized, and runs every case five times. It writes to `out/`, or to the folder `OUT` names. |

Run it with `bash learnability/phase7/preflight/http-in-mo/run.sh` for the first measurement, and
with `BASE=HEAD OUT=out-faster-parser` in front for the second, which was taken at 4968cc2d. It
takes about ten minutes, and uses port 18123 unless `PORT` is set.

## What this does not show

- The connection is a hack inside the `Files` capability. It has no timeout, no name lookup and
  no error cases. The real capability is new code in `src/host/`.
- The client is 101 lines that handle one request. It reads the response until the server closes
  the connection, and it does not check `Content-Length` against what arrived.
- No TLS. Encryption sits under the connection and is C, so it adds no interpreter time.
- One host and one body shape. A body of long strings would cost fewer instructions for each
  byte, and a body of short numbers more.
- Nothing here says how fast a parser that reads `Text` or `Bytes` through a new primitive would
  be. That was not built.
