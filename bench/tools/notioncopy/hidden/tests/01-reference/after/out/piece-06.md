**Files**: `read_text(path: Text) : Result(Text, FileError)`, `read_bytes(path: Text) : Result(Bytes, FileError)`, `write_text(path: Text, text: Text)` (replaces the whole file), `append_text(path: Text, text: Text)`, `remove(path: Text)` and `make_folder(folder: Text)`, each `: Result(Unit, FileError)`; `list(folder: Text) : Result(List(Text), FileError)`, the names in the folder, sorted; `exists?(path: Text) : Bool`; `within(folder: Text) : Files`; `read_only : ReadFiles`. Writing or appending into a folder that doesn't exist gives `Error(NotFound(path: path))`; `make_folder` makes the folder and any missing folders above it, and succeeds if the folder already exists. `entries(folder: Text) : Result(List(Entry), FileError)` gives the folder's entries in `list`'s order, each an `Entry(name: Text, kind: EntryKind)` whose kind is `File`, `Folder`, `Link` or `Other`; a link is reported, never followed. `walk(folder: Text) : Result(List(Entry), FileError)` gives every entry below the folder, each named by its path from the folder: a folder's entries in `list`'s order, each folder followed by what's inside it. It doesn't enter links, and stops at the first folder it can't read. A capability from `within` or `read_only` holds its folder open while it's used, and having too many open at once is a bug (13.2). `Files.in_memory(files: Map(Text, Text)) : Files`. In such a fake, the folders are those its files are in, those `make_folder` made, and those whose last file was removed, as on a disk, and `list` and `entries` give the names directly inside a folder, each subfolder once. Files that make one path both a file and a folder, as `{"a": "x", "a/b": "y"}` do, are a bug (13.2). So are keys that aren't plain relative paths, such as `"/a"`, `"./a"`, `"a/"` or `"a//b"`. **ReadFiles** has `read_text`, `read_bytes`, `list`, `entries`, `walk`, `exists?` and `within`. Paths are relative and use `/`; `"."` is the capability's own folder, and no path can leave it. Given a path that would leave the folder, such as `"../a.txt"`, reading, writing, removing and listing give `Error(NotAllowed(path: path))`, and `exists?` gives `false`. Calling `within` with such a path gives a capability that reaches nothing: every call on it that returns a `Result` gives `Error(NotAllowed(path: path))`, and `exists?` on it gives `false`. A path may pass through symbolic links that stay inside the folder, and every operation follows them; a path through an absolute link, or one that leads outside, is refused in the same way, and one through more than 40 links gives `Error(Failed(path: path, reason: "too many links"))`. `remove` removes a link itself, never what it points to. Listing a file, or making a folder where a file is, gives `Error(Failed(path: path, reason: "not a folder"))`, and reading, writing, appending to or removing a folder gives `Error(Failed(path: path, reason: "is a folder"))`.
**FileError**: `NotFound(path: Text)`, `NotAllowed(path: Text)`, `NotText(path: Text)`, when `read_text` finds a file that isn't valid UTF-8, and `Failed(path: Text, reason: Text)`. Its text forms are `PATH: no such file`, `PATH: not allowed`, `PATH: not text` and `PATH: REASON`, as in `notes/a.md: no such file`.
**Clock**: `now : Time`, a bug if the system's clock is outside the years 1 to 9999; `Clock.fixed(time: Time) : Clock`. **Random**: `int(low: Int, high: Int) : Int`, from `low` up to, but not including, `high`, which must be greater; `Random.seeded(seed: Int) : Random`. **Args** and **Env** fakes: `Args.of(list: List(Text))`, `Env.of(map: Map(Text, Text))`.
**Json**, with `use Json`: `Json.parse(text: Text) : Result(Json, JsonError)` reads JSON as RFC 8259 defines it, into this type:
```ruby
enum Json
  Null
  Boolean(Bool)
  Number(JsonNumber)
  String(Text)
  Array(List(Json))
  Object(Map(Text, Json))
end
```
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
## Appendix B. Grammar
The executable grammar, `grammar/mo.y`, is the authority, and this is its summary. `x*` means zero or more, `x+` one or more, `x?` optional, and `NL` a line break as section 2.4 defines it.
```plain text
module      = "module" Path NL ("expose" exposed ("," exposed)* NL)? use* item*
exposed     = name | Name | Name "(" ".." ")"
use         = "use" Path ("{" (name | Name) ("," (name | Name))* "}")? NL
item        = function | struct | enum | test | name "=" expr NL
function    = "fn" name "(" params? ")" (":" type)? where? NL (promise | examples)* statement* "end" NL
params      = name ":" type ("," name ":" type)*
where       = "where" Name ":" Name ("," Name ":" Name)*
type        = Name ("." Name)? ("(" type ("," type)* ")")?
            | "(" type ("," type)+ ")"
            | "fn" "(" (type ("," type)*)? ")" (":" type)?
struct      = "struct" Name ("(" Name ("," Name)* ")")? NL (name ":" type NL)+ "end" NL
enum        = "enum" Name ("(" Name ("," Name)* ")")? NL (case NL)+ "end" NL
case        = Name | Name "(" type ")" | Name "(" name ":" type ("," name ":" type)* ")"
promise     = "promise" text NL (expr NL)+ (("allows" | "forbids") anchor ("," anchor)* NL)+ "end" NL
anchor      = name ":" expr
examples    = "examples" NL (expr NL)+ "end" NL
test        = "test" text NL statement* "end" NL
statement   = simple NL
            | "for" binding "in" expr NL statement* "end" NL
            | "while" expr NL statement* "end" NL
simple      = expr | block_if
            | target "=" (expr | block_if) | target ("+=" | "-=") expr | "var" name "=" (expr | block_if)
            | "return" expr? ("if" expr)? | ("break" | "continue") ("if" expr)? | "assert" expr
block_if    = "if" expr NL statement* ("else" "if" expr NL statement*)* ("else" NL statement*)? "end"
expr        = "if" expr ":" expr "else" (":" expr | expr)        # one-line if; "else if" chains
            | expr ("implies" | "or" | "and") expr | "not" expr
            | expr ("==" | "!=" | "<" | "<=" | ">" | ">=") expr | expr "is" pattern
            | expr ".." expr | expr ("+" | "-" | "*" | "/" | "%") expr | ("-" | "try") expr
            | expr "." (name | Name | digits) | expr "(" (arg ("," arg)*)? ")"
            | literal | name | Name | "(" expr ")" | "(" expr ("," expr)+ ")"
            | "[" (expr ("," expr)*)? "]" | "{" (expr ":" expr ("," expr ":" expr)*)? "}"
            | "case" expr NL arm (arm | statement)* "end"
            | "fn" "(" (name ("," name)*)? ")" (expr | NL statement*) "end"
            | "stop" text
arg         = expr | name ":" expr
arm         = pattern ("|" pattern)* ("if" expr)? ":" simple? NL
target      = name ("." name)* | "(" binding ("," binding)+ ")"
binding     = name | "_" | "(" binding ("," binding)+ ")"
pattern     = "_" | name | literal | "-" number | Name ("." Name)?
            | Name ("." Name)? "(" (pattern | name ":" pattern ("," name ":" pattern)*) ")"
            | "(" pattern ("," pattern)+ ")"
            | "[" (pattern ("," pattern)*)? "]" | "[" (pattern ",")* ".." (name | "_") "]"
```
A function's parameters, a call's arguments, a case's named fields, and tuples, lists and maps may end with a comma.
Operators, from loosest to tightest. Comparisons, `..` and `implies` don't chain; the others do, left to right.
<table header-row="true">
<tr>
<td>Level</td>
<td>Operators</td>
</tr>
<tr>
<td>1</td>
<td>one-line `if ... else ...`</td>
</tr>
<tr>
<td>2</td>
<td>`implies`</td>
</tr>
<tr>
<td>3</td>
<td>`or`</td>
</tr>
<tr>
<td>4</td>
<td>`and`</td>
</tr>
<tr>
<td>5</td>
<td>`not`</td>
</tr>
<tr>
<td>6</td>
<td>`==` `!=` `<` `<=` `>` `>=` `is`</td>
</tr>
<tr>
<td>7</td>
<td>`..`</td>
</tr>
<tr>
<td>8</td>
<td>`+` `-`</td>
</tr>
<tr>
<td>9</td>
<td>`*` `/` `%`</td>
</tr>
<tr>
<td>10</td>
<td>`-` (negation) and `try`</td>
</tr>
<tr>
<td>11</td>
<td>`.name`, `.0`, and calls</td>
</tr>
</table>
