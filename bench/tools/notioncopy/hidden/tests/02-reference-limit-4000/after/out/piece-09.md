<table header-row="true">
<tr>
<td>Ability</td>
<td>What it gives</td>
<td>Customized with</td>
</tr>
<tr>
<td>`Equal`</td>
<td>`==` and `!=`</td>
<td>nothing: values are equal when their fields are</td>
</tr>
<tr>
<td>`Hash`</td>
<td>use as a map key or a set member</td>
<td>nothing: always structural</td>
</tr>
<tr>
<td>`Order`</td>
<td>`<`, `<=`, `>`, `>=`, `sort`, `min` and `max`</td>
<td>`fn compare(a: T, b: T) : Ordering`</td>
</tr>
<tr>
<td>`Text`</td>
<td>a text form, for `#{...}` and printing</td>
<td>`fn to_text(x: T) : Text`</td>
</tr>
<tr>
<td>`Number`</td>
<td>`+`, `-`, `*`, `/`, `%` and `sum`</td>
<td>nothing: only `Int`, `BigInt`, `Decimal` and `Float` have it; times and durations also add and subtract (3.9)</td>
</tr>
</table>
```ruby
enum Ordering
  Less
  Equal
  Greater
end
```
A type with `Hash` or `Order` also has `Equal`. Ability names appear only after `where`, so they don't clash with other names. Records and choice types get `Equal`, `Hash`, `Order` and `Text` automatically whenever all their fields have them. Equality compares field by field; maps compare their entries, and sets their elements, in any order. Order compares fields in the order they are declared, and cases in the order they are declared. Functions and capabilities have no abilities. The built-in types have these:
<table header-row="true">
<tr>
<td>Type</td>
<td>`Equal`</td>
<td>`Hash`</td>
<td>`Order`</td>
<td>`Text`</td>
<td>`Number`</td>
</tr>
<tr>
<td>`Bool`</td>
<td>yes</td>
<td>yes</td>
<td>`false` first</td>
<td>yes</td>
<td>no</td>
</tr>
<tr>
<td>`Int`, `BigInt`, `Decimal`</td>
<td>yes</td>
<td>yes</td>
<td>yes</td>
<td>yes</td>
<td>yes</td>
</tr>
<tr>
<td>`Byte`</td>
<td>yes</td>
<td>yes</td>
<td>yes</td>
<td>yes</td>
<td>no</td>
</tr>
<tr>
<td>`Float`</td>
<td>IEEE: "not a number" equals nothing</td>
<td>no</td>
<td>yes, with "not a number" last</td>
<td>yes</td>
<td>yes</td>
</tr>
<tr>
<td>`Text`, `Char`</td>
<td>yes</td>
<td>yes</td>
<td>by character code</td>
<td>yes</td>
<td>no</td>
</tr>
<tr>
<td>`Bytes`</td>
<td>yes</td>
<td>yes</td>
<td>byte by byte</td>
<td>no</td>
<td>no</td>
</tr>
<tr>
<td>`Unit`</td>
<td>yes</td>
<td>yes</td>
<td>yes</td>
<td>yes</td>
<td>no</td>
</tr>
<tr>
<td>`Time`, `Duration`</td>
<td>yes</td>
<td>yes</td>
<td>yes</td>
<td>yes</td>
<td>no (but they add and subtract, 3.9)</td>
</tr>
<tr>
<td>`Range`</td>
<td>yes</td>
<td>yes</td>
<td>no</td>
<td>yes: `0..3`</td>
<td>no</td>
</tr>
<tr>
<td>tuples</td>
<td>when every part has it</td>
<td>same</td>
<td>same, part by part from the left</td>
<td>same</td>
<td>no</td>
</tr>
<tr>
<td>`List(T)`</td>
<td>when `T` has it</td>
<td>same</td>
<td>same, element by element; a list comes before any longer list that begins with it: `[1, 2] < [1, 2, 0]`</td>
<td>same</td>
<td>no</td>
</tr>
<tr>
<td>`Map(K, V)`, `Set(T)`</td>
<td>when their keys and values have it, comparing entries in any order</td>
<td>same</td>
<td>no</td>
<td>same</td>
<td>no</td>
</tr>
</table>
`Option`, `Result` and `Ordering` are choice types, so their cases are ordered as they are declared: `None`, `Some`; `Ok`, `Error`; `Less`, `Equal`, `Greater`. `Float` follows IEEE rules: "not a number" equals nothing and sorts last, and floats have no `Hash`, so they can't be map keys.
