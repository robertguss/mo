<table_of_contents/>
**Version 0.1, approved on 12 October 2026.** This is core Mo, the language that later versions build services, live upgrades and approved intent on. It is written to be read in full, by people and by agents, before writing Mo.
Every rule has an example, and every code example parses with Mo's executable grammar (`grammar/` in this repository). "An error" below means the compiler rejects the program. "Stops" means a running program has hit a bug and ends with a report (section 13.2).
## 1. A first program
```ruby
## Prints each word given on the command line, in capitals.
module Shout

fn main(args: Args, console: Console)
  for word in args.list
    console.print(word.ascii_uppercase)
  end
end
```
- A file is one module, named on its first line of code. `##` comments describing it may come before.
- `main` lists the capabilities the program needs: here its arguments and the console. They are its whole authority. It can't touch files, the network or the clock, because it wasn't handed them (section 11).
- Blocks open with a keyword and close with `end`. Indentation carries no meaning; `mo format` lays code out with two spaces per level, in lines of at most 100 columns.
## 2. Source text
### 2.1 Files
A program is a tree of modules under one root folder. The module `Shop.OrderBook` lives in `shop/order-book.mo`: each part of its name becomes a lowercase folder or file name, with hyphens between words. A word starts at a capital letter that follows a lowercase letter or a digit, and at the last capital of a run that a lowercase letter follows, so `XMLReader` lives in `xml-reader.mo`, `HTTP2Server` in `http2-server.mo` and `Unicode16Tool` in `unicode16-tool.mo`.
Source is UTF-8. Characters that change how text looks without being seen are an error anywhere, even in text and comments: direction overrides, zero-width characters, and control characters other than tab and line breaks. In text, write them as escapes, such as `"\u{200B}"`.
### 2.2 Comments
`#` starts a comment that runs to the end of the line. `##` documents the declaration below it, or the module when it comes before `module`. `mo format` lines up the comments at the ends of consecutive lines in one column, two spaces after the longest line's code.
```ruby
## The price of an order, with tax.
fn total(order: Order) : Decimal
  order.subtotal + order.tax  # the tax is already rounded
end
```
### 2.3 Names
- Values, functions, fields and parameters are snake_case: lowercase ASCII letters, digits and `_`, starting with a letter, or with `_` for a name that is unused on purpose.
- Types, the cases of choice types, and modules are CapCase: `OrderBook`, `NotFound`.
- A type parameter is a single capital letter: `T`, `K`, `V`.
- A name ending in `?` is a yes-or-no answer: a `Bool`, or a function that returns one, such as `empty?` or `paid?`.
- `_` alone means "ignored", and a name starting with `_`, such as `_reason`, is unused on purpose.
Names are ASCII; text and comments may use any Unicode. These words are reserved:
```plain text
allows and assert break case continue else end enum examples expose false fn
for forbids if implies in is module not or promise return stop struct test true
try use var where while
```
