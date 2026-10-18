An object keeps its keys in the document's order, and a key given twice is an error. A `JsonNumber` keeps the number as it was written, and two are equal when they're written the same: its `to_int : Option(Int)`, `to_decimal : Option(Decimal)` and `to_float : Option(Float)` each give `None` when the number doesn't fit, and `to_text : Text` gives it as written. `Json.int(n: Int)` and `Json.decimal(d: Decimal)` make a number, and `Json.float(f: Float) : Option(Json)` gives `None` for not-a-number and the infinities, which JSON can't write. `json.to_text` writes compact JSON, and `json.pretty` indents it by two spaces; both keep each object's order and each number's text. A `JsonError(line: Int, column: Int, problem: JsonProblem)` says where reading stopped, counting both from 1, and why: `UnexpectedEnd`, `Unexpected(found: Char)`, `BadNumber`, `BadEscape` (including a lone surrogate in a `\u` escape), `DuplicateKey(key: Text)`, or `TooDeep`, for arrays and objects nested more than 128 deep.
**Unicode16**, following Unicode 16.0: `graphemes(text: Text) : List(Text)`, `display_width(text: Text) : Int`, and `normalize_nfc`, `lowercase` and `uppercase(text: Text) : Text`. `display_width` counts each user-perceived character, as `graphemes` splits the text: 2 columns for an emoji (with its selector, skin tone or joins), a flag, or one holding a wide or fullwidth character; 0 for one made only of control characters, marks and invisible characters; and 1 otherwise, ambiguous characters and a lone flag letter included. A later runtime adds `Unicode17` beside it, so what a program computes changes only when its `use` line does.
## 15. What comes later
Processes, messages and supervision, and tool schemas derived from types (Phase 3); runtime introspection and replay (Phase 4); live upgrades (Phase 5); approving and locking promises (Phase 6); demanding that a function update its input in place (Phase 7); packages and user-defined abilities (Phase 8). None of them changes a rule in this reference.
## Appendix A. Errors
Every error has a headline in this reference's words, the code with the exact spot marked, one reason citing the rule's section, and the fix:
```plain text
error E0412: a failure is ignored                        shop/orders.mo:14:3

   14 │   files.write_text("orders.txt", report)
      │   ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^ returns Result(Unit, FileError), and the result is dropped

Mo never drops a failure silently (reference 8.3). Pass it upward, or handle it:

Fix:
   14 │   try files.write_text("orders.txt", report)
```
A code such as `E0412` never changes its meaning. `mo check --json` gives each error as JSON (section 13.4): its code, headline and reason, the section the reason cites, the places it marks, as byte offsets and as lines and columns counted in characters, and its fixes, each one or more text edits applied together. `mo fix` applies a fix when there is only one. Errors are reported in source order, without the follow-on errors one mistake causes. There are no warnings: a problem is an error, or it isn't reported.
