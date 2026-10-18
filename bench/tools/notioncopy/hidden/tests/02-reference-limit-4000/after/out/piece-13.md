### 13.1 Evaluation
Mo evaluates eagerly, left to right: a call's arguments, then the call; an operator's left side, then its right. `and`, `or` and `implies` skip their right side when the left decides the answer, and `x or default` on an `Option` computes `default` only when `x` is `None`. `try` returns early, and `stop` stops the program.
### 13.2 Bugs
A bug stops the program at once, with a report of what happened and where, and exit status 70. These are bugs: overflow in `Int`, `Decimal` or `Duration` arithmetic, or a time outside the years 1 to 9999; dividing by zero; reaching a `stop`; using more stack, call depth or memory than section 13.4's limits allow, or more memory than the machine has; calling a library function outside what it accepts, such as `take(-1)` or `random.int(5, 5)`; two keys of a map literal that turn out equal (section 3.8); an exit status outside 0 to 255; printing to an output that is gone; having too many folders open at once; and a fake whose files make one path both a file and a folder (section 14). Bugs are never caught.
### 13.3 Costs
Mo promises what operations cost, not how values are stored:
<table header-row="true">
<tr>
<td>Operation</td>
<td>Cost</td>
</tr>
<tr>
<td>`get`, `size`, `first` and `last` on a list</td>
<td>constant</td>
</tr>
<tr>
<td>`append` on a list, and assigning a `var`'s field</td>
<td>constant when the value has one holder; a copy when it's shared</td>
</tr>
<tr>
<td>`get` on a map, and `has?` on a map or a set</td>
<td>constant on average</td>
</tr>
<tr>
<td>`set` and `remove` on a map, and `add` and `remove` on a set</td>
<td>constant on average when the value has one holder; a copy when it's shared</td>
</tr>
<tr>
<td>`byte_count`</td>
<td>constant</td>
</tr>
<tr>
<td>`char_count`, `split`, `lines`, `contains?`</td>
<td>proportional to the text's length</td>
</tr>
<tr>
<td>`sort`, `sort_by` and `sort_by_desc`</td>
<td>n log n, and stable: equal keys keep their order</td>
</tr>
</table>
A value is shared when two names, fields or collections hold it. Changing a value with one holder reuses its memory; changing a shared value copies it first, and its other holders keep the old value.
