### 3.2 Whole numbers
`Int` is a 64-bit signed whole number, and the type of counts, sizes and positions. Literals may put `_` between digits, and may be hexadecimal or binary:
```ruby
population = 8_100_000_000
mask = 0xFF
flags = 0b1010
```
`+`, `-`, `*`, `/` and `%` take two numbers of the same type. `/` rounds toward zero, and `%` gives the remainder with the sign of the left side: `-7 / 2` is `-3`, and `-7 % 2` is `-1`. `a.floor_div(b)` and `a.floor_mod(b)` round toward negative infinity instead.
Overflow, and dividing by zero, are bugs: the program stops. Code that expects a result might not fit says what it wants instead:
```ruby
fits = a.checked_add(b)            # an Option(Int): None if it doesn't fit
hash = h.wrapping_mul(31)          # wraps around
capped = level.saturating_add(10)  # caps at the largest Int
```
`BigInt` holds whole numbers of any size, with the same arithmetic: `n.to_big` converts to one, and `big.to_int` returns `None` when the value doesn't fit. `Byte` holds 0 to 255, for binary data; `n.to_byte` returns an `Option(Byte)`. `Byte` has no arithmetic (section 9.2): convert it with `to_int` first.
### 3.3 Decimal and Float
A literal with a decimal point, such as `19.99`, is a `Decimal`: an exact fraction with 18 decimal places, up to about ±1.7 × 10²⁰. Decimal arithmetic is exact while results fit in 18 places, so `0.1 + 0.2 == 0.3`. A result that needs more places, such as `1.00 / 3`, is rounded to the nearest 18th place, with an exact tie rounded to the even digit. Overflow and dividing by zero stop the program, as with `Int`, and `%` gives the remainder with the sign of the left side, as it does for `Int`. A decimal's text form is its shortest exact spelling: `15.5`, `3`, `0.001`.
`Float` is a 64-bit IEEE 754 binary floating-point number, for science and graphics. A number literal takes the type its context needs: in `x * 0.5`, with `x` a `Float`, `0.5` is a `Float`, and in `price * 2`, with `price` a `Decimal`, `2` is a `Decimal`. A literal with nothing to decide its type is an `Int`, or a `Decimal` if it has a decimal point. Dividing an `Int` by an `Int` gives an `Int`, so convert first to get a fraction: `total.to_decimal / count.to_decimal`. Converting is explicit:
```ruby
share = cents.to_decimal / 100  # Int to Decimal
ratio = share.to_float          # Decimal to Float, rounded to the nearest float
back = ratio.to_decimal         # an Option: a float may be infinite or not a number
rounded = share.round(2)        # 2 places, ties away from zero, as people round by hand
label = rounded.to_fixed(2)     # the text "0.10": exactly 2 places, rounded as round does
```
