**tally:** Reports how often agents fixed Mo's compile errors on their next try. Usage: `tally LOGS`, where LOGS is a folder holding one folder per session. It prints a table, tab-separated, and exits with 0; a usage error or a LOGS that can't be read is reported on standard error and the exit status is 2; a line of `output.jsonl` that isn't JSON is reported on standard error as `PATH:LINE: not JSON`, that run is skipped as if it hadn't happened, and the exit status is 1 once the table is printed.

**The session log.** A session's log is a folder, made by the harness that runs the session (not by the agent). Each run of `mo check`, `mo test` or `mo fix` in the session is a folder inside it, named by its number with three digits (`001`, `002`, ...), holding:

- `command`: one line, `check`, `test` or `fix`;
- `output.jsonl`: for `check` and `test`, exactly what `mo check --json` or `mo test --json` printed (the format of reference 13.4: a `version` record, `diagnostic` records, `result` records and `totals`);
- `src/`: a copy of the program's `.mo` files as they were when it ran.

The harness records `--json` output whatever the agent typed, so the agent sees the usual text.

The rules:

1. **Runs.** A session's runs are taken in the order of their numbers. Only `check` and `test` runs report errors; a `fix` run reports none, and only marks what follows it.
2. **Errors.** Each `diagnostic` record in a run is an error, identified by its `code`, the `file` of its `primary` span, and its function. Its *function* is found in that run's `src/` copy of the file: the last line at or before the span's `line` that starts, at its first column, with `fn ` or `test `. For `fn`, the function is the word after `fn`, up to `(` or `[`; for `test`, it is the whole line, trimmed. With no such line it is `(module)`. Two errors with the same code, file and function in one run are one error.
3. **When an error starts.** An error starts at a run that reports it when the session's previous `check` or `test` run didn't report it, or there was none.
4. **Attempts.** From its start, count the later `check` and `test` runs up to the first one that doesn't report it. That count is the number of attempts it took. When the session ends first, it is *unfixed*.
5. **Fixed by `mo fix`.** When a `fix` run comes between the error's last report and the run that no longer reports it, the error is counted as *by fix* instead, and not in the attempts columns.
6. **The table.** A header line `code errors first_retry within_three by_fix unfixed`, then one line per error code, in code order, then a `total` line. `errors` counts started errors; `first_retry` those that took one attempt; `within_three` those that took one to three; `by_fix` and `unfixed` as above. Tabs separate the columns.
7. **The rates.** Then three lines: `first_retry_rate`, `within_three_rate`, each the total count over (`errors` minus `by_fix`), as a percentage with one decimal place, rounding halves up, such as `86.5%`, or `n/a` when that is 0; and `tests_passed`, from each session's last `test` run's `totals`, summing `passed` and `passed + failed + stopped` across sessions, written `40/44 90.9%`, or `0/0 n/a`.
8. **Records it doesn't know** in `output.jsonl`, and fields it doesn't need, are skipped, as reference 13.4 says a reader should.

## Example

One session, three runs. `001` is `check`, reporting `E0050` in `f` and `E0078` in `g`. `002` is `check`, reporting `E0078` in `g`. `003` is `test`, reporting nothing, with totals of 3 passed and 1 failed. The output is (tabs shown as spaces):

```text
code   errors  first_retry  within_three  by_fix  unfixed
E0050  1       1            1             0       0
E0078  1       0            1             0       0
total  2       1            2             0       0
first_retry_rate    50.0%
within_three_rate   100.0%
tests_passed        3/4 75.0%
```
