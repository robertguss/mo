**stats** (extend a program): Your folder holds the module `Stats` in `stats.mo`, which gives summary statistics of lists of decimal numbers, and its tests, the module `StatsTest` in `stats-test.mo`. Add two functions to `Stats`, and expose them, keeping the four it has as they are:

- `median(values: List(Decimal)) : Option(Decimal)`: the middle value once the values are sorted, or, for an even count, the mean of the two middle values, exactly, without rounding; `None` for an empty list.
- `modes(values: List(Decimal)) : List(Decimal)`: the values that appear most often, each once, in increasing order. When several values are tied for most often, they are all modes; values that are equal, such as `1.50` and `1.5`, are the same value. An empty list has no modes.

Give both functions examples, and add tests of them to `stats-test.mo`.
