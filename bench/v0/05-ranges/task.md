**ranges** (write a program): Write a program, the module `Ranges` in the file `ranges.mo`, that reads ranges of whole numbers from standard input, one on each line, and prints the fewest ranges that cover exactly the same numbers. Usage: `ranges [--gaps]`.

- A line ends at `\n` or `\r\n`, and the last may have no line ending. Spaces and tabs at the start and end of a line are ignored, and a line that is then empty is skipped.
- A range is `LOW-HIGH`, the numbers from LOW to HIGH with both included, or a single number `N`, the range of just N. A number is one or more ASCII digits, nothing else, and fits in a 64-bit signed integer; leading zeros are allowed. LOW must not be greater than HIGH.
- Any other line is reported on standard error as `line N: not a range: TEXT`, where N counts every line from 1 and TEXT is the line without the spaces and tabs at its ends. The line is skipped, and the exit status is then 1; otherwise it is 0.
- Ranges that overlap or touch are merged into one: `3-5` and `6-8` touch, so they become `3-8`. The program prints the merged ranges in increasing order, one on each line, written `LOW-HIGH`, or `N` for a range of one number.
- With `--gaps`, it prints instead the ranges of numbers that lie between the merged ranges and are in none of them, in the same form and order. Anything else on the command line is a usage error: the program prints `usage: ranges [--gaps]` on standard error, prints nothing else, and exits with status 2.

For example, given

```text
10-12
3-5
6-8
15
11-14
```

it prints `3-8` and `10-15`, each on its own line, and with `--gaps` it prints `9`.
