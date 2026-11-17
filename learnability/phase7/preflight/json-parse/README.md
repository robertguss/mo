# A faster `Json.parse`, still in Mo

The first measurement of [HTTP written in Mo](../http-in-mo/README.md) missed its wall-time bar
because of `Json.parse`. Robert asked for a faster parser in the same step. Commit 4968cc2d
changes `lib/json.mo`, and this folder compares the parser before it (2b6335c3) with the parser
after it.

The parser is still written in Mo. It uses no new primitive.

## What changed

The old parser turned the text into a `List(Char)` and read every string one character at a
time.

The new parser also cuts the text at its quotes, with `text.split("\"")`. A string with no
escape and no control character is exactly one of those pieces, so the parser takes the piece
whole. It counts the quotes it has passed, and that count names the piece after an opening quote.

Any other string takes the walk it took before. That walk also counts the quotes it passes, so
the strings after it are still found. Because the old walk produces every error, every error
and its position are unchanged.

## Results

Debug build, three runs each, user time of the whole `mo run` (median):

| Document | Bytes | Old parser | New parser |
| --- | --- | --- | --- |
| The 1 MiB body of the HTTP measurement | 1,048,867 | 5.25 s, 109,677,820 instructions | 1.65 s, 28,250,591 instructions |
| The same, printed with an indent of two | 1,409,048 | 5.69 s, 120,834,050 | 2.86 s, 57,525,583 |
| The same, with an escape in every string | 1,656,475 | 9.64 s, 201,069,580 | 8.91 s, 181,741,175 |
| 30,000 rows of four numbers | 1,166,790 | 8.42 s, 189,478,773 | 8.10 s, 166,980,520 |

No document got slower. The gain is for strings with no escape. Numbers and strings with escapes
still go one character at a time.

The heap peak of the first row grows from 37.0 MiB to 41.7 MiB, because the pieces are held
beside the list of characters. The other rows grow by less than 6 MiB or not at all.

Every row is in [out/results.txt](out/results.txt).

## The two parsers give the same answers

`corpus.py` writes 8,111 documents: hand-written cases, every truncation of a small document,
one character replaced or inserted at every position, and 3,000 random mutations from a fixed
seed. Most are malformed. `differ.mo` prints `Json.parse` of each one, value or error with its
line and column.

Both parsers print the same line for every document. 1,504 of the lines are values and the rest
are errors.

`lib/json.mo` also gained one test for the case the quote count can get wrong: strings after a
string with an escaped quote, with `"`, and with an error. That test fails when the count
for an escaped quote is removed.

## What it took, and what that says about the VM

Instruction counts for the 1 MiB body, taken once each in a scratch tree while the change was
made. The scratch tree is not kept.

| Version | Instructions |
| --- | --- |
| The old parser | 109,677,820 |
| A map from the position of each opening quote to its piece | 60,394,489 |
| The same, with a cheaper loop to build the map | 51,756,330 |
| A two-line `skip_space` for the usual case, and a direct path for a value that is a string | 32,926,602 |
| A count of quotes in place of the map | 30,223,751 |
| The version that was committed | 28,250,591 |

A throwaway counter of instructions for each function found where the cost was. Three things
it showed are for the phase about speed, because they are about the code the compiler writes
and not about JSON:

- A call costs by the size of the function, not by what it does. The old `skip_space` was a
  `while true` loop around a `case`. Each call ran 116 instructions, 62 of them `const`, even
  when the next character was not a space. A function of one comparison ran 23.
- A `for` loop over a list ran about 62 instructions for each item, 28 of them `clear`.
- The debug build ran 16 to 20 million instructions a second for all of these.

## What was not tried

- A new primitive that scans text. It is the next step for numbers and for strings with escapes.
- A change to how numbers or escapes are read.
- A parser that does not build the `List(Char)` at all. The old walk and the positions in errors
  need it.

## Files

| File | What it is |
| --- | --- |
| `corpus.py` | Writes the corpus, one document after another with the byte 0x1D between them. |
| `differ.mo` | Prints the index and the result of `Json.parse` for each document of the corpus. |
| `shapes.py` | Writes the three other shapes of the 1 MiB body. |
| `run.sh` | Exports both commits (2b6335c3 and the one checked out, or `OLD` and `NEW`), builds each with the HTTP prototype's patch for its instruction counter, compares the two parsers on the corpus, and times `parse.mo` on each shape. |

Run it with `bash learnability/phase7/preflight/json-parse/run.sh`. It takes about three minutes.
