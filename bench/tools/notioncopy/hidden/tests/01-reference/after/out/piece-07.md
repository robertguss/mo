## Appendix C. Habits from other languages
<table header-row="true">
<tr>
<td>Elsewhere</td>
<td>In Mo</td>
<td>Section</td>
</tr>
<tr>
<td>`items[i]`, `map[key]`</td>
<td>`items.get(i)`, `map.get(key)`, each an `Option`</td>
<td>3.7, 3.8</td>
</tr>
<tr>
<td>`len(x)`, `x.length`</td>
<td>`list.size`; `text.byte_count` or `text.char_count`</td>
<td>3.4, 14</td>
</tr>
<tr>
<td>`null`, `nil`, `None` checks</td>
<td>`Option`: `x or default`, `case`, `is`</td>
<td>8.1</td>
</tr>
<tr>
<td>`throw`, `raise`, `try ... catch`</td>
<td>return `Error(...)`; pass it on with `try`; handle it with `case`</td>
<td>8.2, 8.3</td>
</tr>
<tr>
<td>`unwrap()`, `!`</td>
<td>`case`, with `stop "why it can't happen"`</td>
<td>8.4</td>
</tr>
<tr>
<td>`!x`, `&&`, `||`</td>
<td>`not x`, `and`, `or`</td>
<td>3.1</td>
</tr>
<tr>
<td>`elif`, `elsif`</td>
<td>`else if`</td>
<td>7.1</td>
</tr>
<tr>
<td>`cond ? a : b`</td>
<td>`if cond: a else: b`</td>
<td>7.1</td>
</tr>
<tr>
<td>`match`, `switch`, `=>`</td>
<td>`case`, with arms written `Pattern: ...`</td>
<td>7.2</td>
</tr>
<tr>
<td>`x => ...`, `|x| ...`, `lambda x: ...`</td>
<td>`fn(x) ... end`</td>
<td>5.6</td>
</tr>
<tr>
<td>a closure that reads a variable assigned later</td>
<td>copy the value into a name first, `current = count`, or pass it as a parameter</td>
<td>5.6</td>
</tr>
<tr>
<td>`let x`, `const x`, `x := ...`</td>
<td>`x = ...`, or `var x = ...` to assign it again</td>
<td>6</td>
</tr>
<tr>
<td>`list.push(x)`, changing a list in place</td>
<td>`items = items.append(x)`, with `var items`</td>
<td>3.7, 6.2</td>
</tr>
<tr>
<td>`x = x.trim()` on a fixed name</td>
<td>`trimmed = x.trim`</td>
<td>6.1</td>
</tr>
<tr>
<td>`print(x)`, `console.log(x)`</td>
<td>`console.print("#{x}")`, with a `Console` from `main`</td>
<td>11, 14</td>
</tr>
<tr>
<td>`"a" + b`</td>
<td>`"a#{b}"`</td>
<td>3.4</td>
</tr>
<tr>
<td>`x.f()` with no arguments</td>
<td>`x.f`</td>
<td>5.4</td>
</tr>
<tr>
<td>`str(x)`, `x.toString()`</td>
<td>`x.to_text`, or `"#{x}"` inside text</td>
<td>9.2</td>
</tr>
<tr>
<td>`def`, `function`, `func`</td>
<td>`fn name(x: T) : R ... end`</td>
<td>5.1</td>
</tr>
<tr>
<td>`class`, methods, `self`</td>
<td>`struct` or `enum`, with functions in its module</td>
<td>4, 5.4</td>
</tr>
<tr>
<td>`import x`, `export`</td>
<td>`use A.B{x}`, and the `expose` line</td>
<td>10</td>
</tr>
<tr>
<td>`List<T>`, `Vec<T>`</td>
<td>`List(T)`</td>
<td>9.1</td>
</tr>
<tr>
<td>`->` before a result type</td>
<td>`:`</td>
<td>5.1</td>
</tr>
<tr>
<td>`;`</td>
<td>a line break</td>
<td>2.4</td>
</tr>
</table>
