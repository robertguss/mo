### 3.4 Text, Char and Bytes
`Text` is Unicode text. It is stored as UTF-8 and is always valid. Text is written in double quotes, with the escapes `\n`, `\t`, `\r`, `\\`, `\"`, `\'`, `\#` and `\u{1F600}` (a character by its hexadecimal code). `#{...}` inside text inserts the text form of any value that has one (section 9.2), and may hold any expression, even one with text in it, as in `"#{name or "you"}"`:
```ruby
greeting = "Hello, #{name}! You have #{count} new messages."
literal = "write \#{name} to insert a name"
```
Text on several lines starts with `"""` at the end of a line and ends with `"""` first on a line, after its indentation; code may go on after it on that line, as in `""")`. It holds the lines between them, without the line break before the closing `"""`, which sets the indentation every line drops:
```ruby
usage = """
  Usage: report [--json] FILE
  Prints a summary of FILE.
  """
```
A `Char` is one Unicode character (a code point), in single quotes: `'a'`, `'\n'`, `'é'`.
Text has no single "length". `text.byte_count` counts UTF-8 bytes, instantly; `text.char_count` counts characters, reading the text. There is no `text[i]`: use whole-text operations such as `split`, `lines`, `trim`, `starts_with?` and `replace`, or walk `text.chars`. Two texts are equal when their bytes are equal, and texts sort by character code. Mo never normalizes text silently. The `Unicode16` module (section 14) normalizes text, splits it into user-perceived characters, and converts case beyond ASCII.
`Bytes` is binary data. `text.bytes` always succeeds; `bytes.decode_utf8` returns `None` when the bytes aren't valid UTF-8.
### 3.5 Unit
`Unit` is the type with one value, also written `Unit`. A function with no result type returns it. `Result(Unit, E)` is the result of an action that can fail but has nothing to return.
### 3.6 Tuples
A tuple groups two or more values: `(3, "three")` has type `(Int, Text)`. `pair.0` and `pair.1` read its parts, and a pattern takes it apart:
```ruby
(low, high) = bounds(readings)
spread = high - low
```
### 3.7 Lists
`List(T)` is Mo's sequence: `[1, 2, 3]`, or `[]` when empty. Every element has the same type. Reading by position returns an `Option`, because the position may be past the end:
```ruby
first = items.first       # an Option
third = items.get(2)      # positions start at 0
longer = items.append(4)  # a new list; items itself is unchanged
```
Functions that "change" a list return a new one, so `items.append(4)` on a line of its own is an error: the new list is unused. With a `var`, write `items = items.append(4)`. When the old list has no other holder, the new one reuses its memory, so building a list in a loop costs what it would with a mutable array (section 13.3).
### 3.8 Maps and sets
`Map(K, V)` maps keys to values: `{"apples": 3, "pears": 5}`, or `{}` when empty. Keys are expressions, so `{fruit: 3}` uses the value of `fruit`. A literal that gives the same key twice is an error when the compiler can see it, as with two equal literals or the same name twice, and a bug when two keys turn out equal as the program runs. `Set(T)` holds distinct values: `Set.empty`, a constant, or `items.to_set`. Keys and set members need equality and hashing, which most types have (section 9.2).
Maps and sets keep insertion order, also after removals: iterating visits keys in the order they were added, setting an existing key keeps its place, and a key removed and added again goes last. Two maps are equal when they hold the same entries, in any order. Iterating a map gives `(key, value)` tuples.
```ruby
stock = {"apples": 3}
more = stock.set("pears", 5)
count = more.get("apples") or 0  # get returns an Option
for (fruit, n) in more
  console.print("#{fruit}: #{n}")
end
```
