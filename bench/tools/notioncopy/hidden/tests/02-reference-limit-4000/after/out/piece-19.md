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
