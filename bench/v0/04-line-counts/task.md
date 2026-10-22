**line-counts** (write a program): Write a program, the module `LineCounts` in the file `line-counts.mo`, that reads lines from standard input and prints each distinct line once with how many times it appeared, most frequent first. Usage: `line-counts [--ignore-case] [--top N]`.

- A line ends at `\n` or `\r\n`, which is not part of it, and the last line may have no line ending. Lines are compared exactly as they are, spaces included, and an empty line is a line like any other.
- Each output line is `COUNT LINE`: the count, padded on the left with spaces to the width of the largest count printed, then a space, then the line. Lines are printed most frequent first; lines that appeared equally often keep the order in which each first appeared.
- `--ignore-case` compares lines ignoring ASCII case, so `Error` and `ERROR` are one line, which is printed as it was spelled the first time it appeared. Other characters are compared exactly.
- `--top N` prints only the first N lines of the output, where N is a whole number of at least 1. When there are fewer, it prints them all.
- The options may come in either order. Anything else on the command line, or a `--top` without such an N after it, is a usage error: the program prints `usage: line-counts [--ignore-case] [--top N]` on standard error, prints nothing else, and exits with status 2. Otherwise the exit status is 0.

For example, given

```text
GET /
POST /login
GET /
GET /about
POST /login
GET /
```

it prints

```text
3 GET /
2 POST /login
1 GET /about
```
