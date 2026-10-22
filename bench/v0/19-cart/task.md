**cart** (extend a program): Your folder holds the module `Cart` in `cart.mo`, a shopping cart with its lines, total and receipt, and its tests, the module `CartTest` in `cart-test.mo`. Add discount codes to it, keeping what it does now for a cart without a code:

- `apply_code(cart: Cart, code: Text) : Result(Cart, CodeError)` puts a code on the cart. The codes are `SAVE10`, ten percent off the subtotal; `FIVEOFF`, 5.00 off; and `BULK`, fifteen percent off each line whose quantity is 10 or more. Codes are matched exactly, so `save10` is not a code. A cart holds at most one code.
- `CodeError` is a choice type with the cases `UnknownCode(code: Text)`, giving the code that isn't one, and `AlreadyApplied(code: Text)`, giving the code the cart already holds. Applying an unknown code gives `UnknownCode` whether or not the cart holds one.
- `discount(cart: Cart) : Decimal` is what the cart's code takes off: computed from the cart's lines as they are, including lines added after the code, rounded once to whole cents, a half away from zero, and never more than the subtotal. A cart without a code has a discount of 0.
- `total` becomes the subtotal less the discount. `receipt` gains a line `discount CODE: -AMOUNT`, with AMOUNT written with two decimal places as the other amounts are, just before the total, on a cart with a code.

Expose the new names, `CodeError` with its cases, and add tests of the codes to `cart-test.mo`.
