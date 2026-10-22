**brackets** (write a program): Write a program, the module `Brackets` in the file `brackets.mo`, that checks that the brackets in each file named on the command line balance. Usage: `brackets FILE...`, with paths relative to the current folder.

- The brackets are `(` and `)`, `[` and `]`, and `{` and `}`. Every other character is ignored, wherever it is: the program doesn't know about strings or comments. The brackets balance when each closing bracket closes the innermost bracket still open, which must be of its kind, and none is left open at the end.
- For each file, in the order given, the program prints one line on standard output: `PATH: ok` when its brackets balance, or else the first problem found reading the file from its start:
  - `PATH:LINE:COLUMN: unexpected C` for a closing bracket `C` with no bracket open;
  - `PATH:LINE:COLUMN: C does not match O at LINE:COLUMN` for a closing bracket `C` that doesn't match the innermost open bracket `O`, giving where each of them is;
  - `PATH: O at LINE:COLUMN is never closed` when the file ends with brackets still open, naming the innermost one.

  PATH is the path as it was given. LINE and COLUMN count from 1; a line ends at `\n`, and the column counts characters, not bytes.
- A file that can't be read is reported on standard error, as `brackets: PATH: no such file` for one that doesn't exist, and the program goes on to the next.
- The exit status is 0 when every file was read and its brackets balance, and 1 otherwise. With no arguments, it prints `usage: brackets FILE...` on standard error, and the exit status is 2.

For example, given `main.c` holding

```text
int main() {
  return f(a[0]);
}
```

and `notes.txt` holding

```text
a (b
c] d
```

`brackets main.c notes.txt` prints

```text
main.c: ok
notes.txt:2:2: ] does not match ( at 1:3
```

and exits with status 1.
