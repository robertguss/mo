# Grading rubric for the learnability test

Frozen with the protocol on 12 Oct 2026. Graders are fresh sessions that didn't
write the reference or the attempt. Each grades on its own; a third settles any
disagreement.

You grade programs written in Mo, a new language, by a model that saw only
Mo's reference, `docs/reference/README.md`. The reference is the only
definition of Mo: judge every program against it alone, as a strict compiler
and a careful reviewer would. There is no compiler, so judge by reading.

For each program, give:

1. **Parses:** given to you from the executable grammar. Don't change it.
2. **Would compile:** list every mistake a compiler following the reference
   would reject, each with its line, what was written, and the section of the
   reference it breaks. Check at least: names and library functions exist with
   the signatures section 14 gives them; types match (an `Option` or `Result`
   used as a plain value, `Int` mixed with `Decimal`, comparing different
   types); failures are handled or passed upward (8.3), and no result other
   than `Unit` is dropped (6.1); names follow 6.1 (no rebinding a visible name,
   nothing unused unless it starts with `_`); only a `var` or its fields are
   assigned (6.2); `case` covers every value, with no whole-arm catch-all over a
   choice type (7.2); capabilities come only from parameters, and fakes only in
   tests, examples and promises (11, 12.4); promises have a sentence, a body,
   and `allows` and `forbids` anchors giving every parameter and `result`
   (12.2). Don't count a mistake twice when one causes another.
3. **Does the task:** whether the program does what its task text asks,
   including exit statuses and output, judged by reading it. Name what's
   missing or wrong.
4. **Verdict:** `pass` (parses, no compile mistakes, does the task), `close`
   (does the task, with at most two compile mistakes that a compiler would
   catch and explain), or `fail`.
5. **Stumbles:** for every mistake, a short line for the diagnostic log: the
   form written, the rule it breaks with its section, and where the form likely
   came from when that's clear (for example "`elif`: Python", "`items[0]`:
   indexing", "`x.len()`: invented library call").

Report as a Markdown table per program (parses, would compile, does the task,
verdict), then the stumbles list. Be exact and terse. Don't fix the programs.
