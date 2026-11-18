# The bar for HTTP written in Mo

Written and committed before any measurement in this folder was taken. The commit that holds
this file holds no result.

## What is decided by it

Phase 7's plan bets that the network capability can work at the level of a connection, with HTTP
parsed in Mo. The cost of that choice is interpreter time. This bar says how much is acceptable.

## The case

One `mo run` of a program that:

1. opens a plain connection to an HTTP server on loopback,
2. sends one `GET` request,
3. reads a response whose body is about 1 MiB of JSON (1,048,576 bytes, give or take 1%),
4. parses the status line, the headers and the body framing in Mo,
5. turns the body into `Text` and calls `Json.parse` on it,
6. prints one value taken from the parsed `Json`, so the parse cannot be skipped.

The body is an array of objects with text, number, boolean and null fields, which is the shape of
a page of results from a listing API. It is served twice: once with `Content-Length` and once with
chunked transfer encoding.

## The bar

| Figure | Must stay under | Measured how |
| --- | --- | --- |
| Wall time of the whole `mo run` | 2.0 seconds | Median of five runs, debug build (`make CC=gcc`), on the development host |
| Heap peak of the Mo process | 256 MiB | The VM's own peak figure for the run |

Both transfer encodings must meet both figures.

## Why these figures

- A spike is an edit, a run and a look. One second keeps a person's train of thought and ten
  seconds loses it. A 1 MiB response is a large one: most API responses are between 1 KiB and
  100 KiB. Two seconds for the large case puts the usual case well under one second if the cost
  grows with the size of the response.
- The wall time is that of the whole command, because that is what the person waits for. It
  includes checking and compiling the program and the library.
- The debug build is the one `make` produces and the one the task queue was deployed with.
- A program's default heap limit is 1 GiB (`MO_DEFAULT_HEAP_SIZE`). A quarter of it for one
  response leaves room to hold the parsed value, work on it and fetch another.

## What a miss means

A miss on either figure, for either encoding, stops the design step. The record then says which
part of the path took the time or the memory, and proposes one of two changes for Robert to
choose: `Bytes` operations that remove the cost, or moving a named part into C with a fuzz
target. The level of the capability is not fixed until he has chosen.

## Also recorded, with no bar

- The same case with a body of about 100 KiB.
- The same case on an optimized build (`DEBUG='-O2 -g -pthread'`, as `bench/performance/` builds
  it).
- The same case using only the `Bytes` operations Mo has today (`size`, `get`, `decode_utf8`),
  to show what the proposed operations buy.
- `Json.parse` alone on the same body read from a file, to separate its cost from HTTP's.
