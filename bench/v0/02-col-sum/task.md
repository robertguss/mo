**col-sum** (write a program): Write a program, the module `ColSum` in the file `col-sum.mo`, that totals each column of a table of numbers read from standard input, in CSV form. It takes no arguments.

- The first line is the header: the columns' names, separated by commas. Every other line is a row of fields, separated by commas, one for each column. Lines end with `\n` or `\r\n`, and the last may have no line ending. Spaces and tabs around a name or a field are not part of it.
- A field is a number: an optional `-`, one or more ASCII digits, and optionally a `.` followed by 1 to 18 digits. So `3`, `-0.25` and `10.50` are numbers, and `3.`, `.5`, `1e3` and the empty field are not.
- A line that is empty or holds only white space is skipped.
- A row with the wrong number of fields is reported on standard error as `line N: expected K fields, found M`; otherwise a row with a field that isn't a number is reported as `line N: not a number: FIELD`, naming the leftmost such field. The row is skipped, and the exit status is then 1. Lines are counted from 1, the header included, and skipped lines count too.
- After the last line, the program prints one line for each column, in the header's order: `NAME: TOTAL`, where TOTAL is the sum of the column's numbers in the rows that weren't skipped, exactly, then written with exactly two decimal places, a half rounded away from zero. A column with no numbers totals `0.00`.
- With no input at all, it prints `no header` on standard error, and the exit status is 1. Otherwise the exit status is 0, unless a row was skipped.

For example, given

```text
item, price, tax
1, 4.50, 0.90
2, 10, 2
```

it prints

```text
item: 3.00
price: 14.50
tax: 2.90
```

and exits with status 0.
