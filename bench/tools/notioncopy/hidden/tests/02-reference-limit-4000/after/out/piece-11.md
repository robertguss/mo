### 10.5 Always in scope
Always in scope: the built-in types of section 3, `Option` with `Some` and `None`, `Result` with `Ok` and `Error`, `Ordering` with its cases, the capability types, `FileError` with its cases, and `Entry` with `EntryKind` and its cases. The functions of section 14 belong to their types' modules: call them with a dot, as in `text.lines`, or through the module, as in `Set.empty` and `Time.parse(text)`. So a local may be called `lines` or `count` without clashing with them.
## 11. Capabilities and effects
### 11.1 What a function can touch
A function can affect or observe the world outside the program only through capabilities it is handed as parameters. A capability's type names what it can do: `Console` prints, `Files` reads and writes files, `Clock` tells the time. So a function's parameter types are its complete list of effects, and a function with no parameter that is or holds a capability only computes: given the same arguments, it always gives the same result.
<table header-row="true">
<tr>
<td>Capability</td>
<td>Lets a program</td>
</tr>
<tr>
<td>`Args`</td>
<td>read its command-line arguments</td>
</tr>
<tr>
<td>`Env`</td>
<td>read environment variables</td>
</tr>
<tr>
<td>`Console`</td>
<td>print to standard output and standard error, and read standard input</td>
</tr>
<tr>
<td>`Files`</td>
<td>read and write files in one folder and the folders inside it</td>
</tr>
<tr>
<td>`ReadFiles`</td>
<td>only read them</td>
</tr>
<tr>
<td>`Clock`</td>
<td>read the current time</td>
</tr>
<tr>
<td>`Random`</td>
<td>draw random numbers</td>
</tr>
</table>
### 11.2 main
`main` lists the capabilities the program needs, in any order and at most one of each type. The runtime hands over exactly those, so a program's whole authority is on its first line. The `Files` or `ReadFiles` it gets reach the folder the program was started in.
`main` returns nothing, and the program exits with status 0; or an `Int`, the exit status, from 0 to 255; or `Result(Unit, E)`, where `Error(e)` prints `e`'s text form to standard error and exits with status 1.
```ruby
## Prints the first line of each file named on the command line.
module Head

fn main(args: Args, console: Console, files: ReadFiles) : Result(Unit, FileError)
  for name in args.list
    text = try files.read_text(name)
    console.print(text.lines.first or "")
  end
  Ok(Unit)
end
```
### 11.3 Passing less
A capability can be narrowed before it is passed on. `files.within("reports")` reaches only that folder, and `files.read_only` returns a `ReadFiles`, which can only read. Calling `within` with a path that would leave the folder, such as `".."`, gives a capability that reaches nothing (section 14). Pass each function the least it needs:
```ruby
summary = try summarize(files.within("reports").read_only)
```
### 11.4 Rules
- Records may hold capabilities, so a function that needs several can take one record.
- Anonymous functions can't capture capabilities, or values holding them. A function that needs one takes it as a parameter.
- Only the runtime makes real capabilities. Tests, examples and promises use fakes (section 12.4).
- Capabilities have no abilities: they can't be compared, printed or used as keys.
- There are no log statements. To report something, print it or return it.
## 12. Examples, promises and tests
A function's specification sits at the top of its body: its promises and examples, then a blank line, then its code.
### 12.1 Examples
`examples` lists calls with their expected results. Each line is a `Bool` expression, usually `call == expected`, and `mo test` checks every line:
```ruby
fn initials(name: Text) : Text
  examples
    initials("Ada Lovelace") == "AL"
    initials("") == ""
  end

  firsts = name.words.filter_map(fn(word) word.chars.first end)
  Text.from_chars(firsts)
end
```
