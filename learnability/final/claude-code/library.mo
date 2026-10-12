## The core of a lending library: a catalog of books and their copies, the
## members who borrow the copies, and the loans between them.
##
## A member has at most three copies out at once, and a copy returned more
## than 14 days after it was borrowed costs 0.25 for each day, or part of a
## day, over. The time is passed in to every step that needs it, so nothing
## here reads a clock: every function only computes.
module Library
expose Library, Book(..), Loan(..), Receipt(..), BorrowError(..), ReturnError(..),
  CatalogError(..), empty, max_loans, loan_period, daily_fine, add_book, add_copy,
  enroll, borrow, return_copy, fine, loans_of, loan_count, available?,
  available_copies, book_of, overdue

## The most copies a member may have out at once.
max_loans = 3

## How long a copy may be kept without a fine.
loan_period = 14.days

## The fine for each day, or part of a day, that a copy is kept past the loan
## period.
daily_fine = 0.25

## A book in the catalog. Members borrow its copies.
struct Book
  isbn: Text
  title: Text
  author: Text
end

## A copy out on loan: who has it, and since when.
struct Loan
  copy: Text
  member: Text
  since: Time
end

## Everything the library knows. Loans are kept by copy, so a copy has at most
## one loan, and `borrow` promises never to replace a loan and never to give a
## member a fourth. The type is exposed without its insides, so only this
## module's functions build a library, and every library keeps those promises.
struct Library
  books: Map(Text, Book)    # each book, by its ISBN
  copies: Map(Text, Text)   # the ISBN of each copy's book, by the copy's id
  members: Set(Text)        # the members' card numbers
  loans: Map(Text, Loan)    # the loan of each copy that is out, by the copy's id
end

## What returning a copy gives: the library without the loan, the loan that
## ended, and the fine for it.
struct Receipt
  library: Library
  loan: Loan
  fine: Decimal
end

## Why a copy can't be lent.
enum BorrowError
  UnknownMember(member: Text)
  UnknownCopy(copy: Text)
  OnLoan(copy: Text)
  LimitReached(member: Text)
end

## Why a copy can't be returned.
enum ReturnError
  NotLent(copy: Text)
end

## Why a copy can't be added to the catalog.
enum CatalogError
  UnknownBook(isbn: Text)
  DuplicateCopy(copy: Text)
end

## A library with no books, copies, members or loans.
empty = Library(books: {}, copies: {}, members: Set.empty, loans: {})

## Adds a book to the catalog, or corrects its title and author when its ISBN
## is there already.
fn add_book(library: Library, book: Book) : Library
  var updated = library
  updated.books = library.books.set(book.isbn, book)
  updated
end

## Adds a copy of a book in the catalog, under the copy's own id, such as the
## barcode on its spine.
fn add_copy(library: Library, copy: Text, isbn: Text) : Result(Library, CatalogError)
  return Error(UnknownBook(isbn: isbn)) if not library.books.has?(isbn)
  return Error(DuplicateCopy(copy: copy)) if library.copies.has?(copy)
  var updated = library
  updated.copies = library.copies.set(copy, isbn)
  Ok(updated)
end

## Enrolls a member under their card number. Enrolling a member again changes
## nothing.
fn enroll(library: Library, member: Text) : Library
  var updated = library
  updated.members = library.members.add(member)
  updated
end

## Lends a copy to a member from `now`: a copy on the shelf, to a member with
## fewer than three copies out.
fn borrow(library: Library, member: Text, copy: Text, now: Time) : Result(Library, BorrowError)
  promise "a copy is never lent to two members at once"
    result is Ok(_) implies not library.loans.has?(copy)
    result is Ok(after) implies
      after.loans.get(copy) == Some(Loan(copy: copy, member: member, since: now))
    result is Ok(after) implies after.loans.remove(copy) == library.loans
    allows library: sample, member: "ana", copy: "dune-1", now: opening,
      result: Ok(lent_to("ana", ["dune-1"]))
    forbids library: lent_to("ben", ["dune-1"]), member: "ana", copy: "dune-1", now: opening,
      result: Ok(lent_to("ana", ["dune-1"]))
  end
  promise "a member never has more than three loans"
    loan_count(library, member) >= max_loans implies result is Error(_)
    result is Ok(after) implies
      after.loans.values.all?(fn(loan) loan_count(after, loan.member) <= max_loans end)
    allows library: lent_to("ana", ["dune-1", "dune-2", "emma-1"]), member: "ana",
      copy: "emma-2", now: opening, result: Error(LimitReached(member: "ana"))
    forbids library: lent_to("ana", ["dune-1", "dune-2", "emma-1"]), member: "ana",
      copy: "emma-2", now: opening,
      result: Ok(lent_to("ana", ["dune-1", "dune-2", "emma-1", "emma-2"]))
  end

  return Error(UnknownMember(member: member)) if not library.members.has?(member)
  return Error(UnknownCopy(copy: copy)) if not library.copies.has?(copy)
  return Error(OnLoan(copy: copy)) if library.loans.has?(copy)
  return Error(LimitReached(member: member)) if loan_count(library, member) >= max_loans
  var updated = library
  updated.loans = library.loans.set(copy, Loan(copy: copy, member: member, since: now))
  Ok(updated)
end

## Takes back a copy that is out, ending its loan, and works out the fine for
## keeping it past the loan period.
fn return_copy(library: Library, copy: Text, now: Time) : Result(Receipt, ReturnError)
  promise "returning ends the copy's loan and leaves every other loan as it was"
    library.loans.has?(copy) or result == Error(NotLent(copy: copy))
    result is Ok(receipt) implies receipt.library.loans == library.loans.remove(copy)
    allows library: lent_to("ana", ["dune-1", "dune-2"]), copy: "dune-1", now: opening,
      result: Ok(Receipt(library: lent_to("ana", ["dune-2"]),
        loan: Loan(copy: "dune-1", member: "ana", since: opening), fine: 0))
    forbids library: lent_to("ana", ["dune-1", "dune-2"]), copy: "dune-1", now: opening,
      result: Ok(Receipt(library: sample,
        loan: Loan(copy: "dune-1", member: "ana", since: opening), fine: 0))
  end

  loan = try library.loans.get(copy).or_fail(NotLent(copy: copy))
  var updated = library
  updated.loans = library.loans.remove(copy)
  Ok(Receipt(library: updated, loan: loan, fine: fine(now - loan.since)))
end

## The fine for a copy kept this long: 0.25 for each day, or part of a day,
## past the 14-day loan period.
fn fine(kept: Duration) : Decimal
  promise "a copy kept up to 14 days costs nothing, and one kept longer costs at least a day's fine"
    kept <= loan_period implies result == 0
    kept > loan_period implies result >= daily_fine
    allows kept: 20.days, result: 1.50
    forbids kept: 14.days, result: 0.25
  end
  examples
    fine(0.days) == 0
    fine(9.days) == 0
    fine(14.days) == 0
    fine(14.days + 1.seconds) == 0.25
    fine(14.days + 23.hours) == 0.25
    fine(15.days) == 0.25
    fine(15.days + 6.hours) == 0.50
    fine(16.days) == 0.50
    fine(21.days) == 1.75
    fine(44.days) == 7.50
  end

  return 0 if kept <= loan_period
  over = kept - loan_period
  days_over = (over.in_seconds / 1.days.in_seconds).ceil   # a part of a day counts as a day
  days_over * daily_fine
end

## The copies a member has out, the oldest loan first.
fn loans_of(library: Library, member: Text) : List(Loan)
  library.loans.values
    .filter(fn(loan) loan.member == member end)
    .sort_by(fn(loan) loan.since end)
end

## How many copies a member has out.
fn loan_count(library: Library, member: Text) : Int
  library.loans.values.count(fn(loan) loan.member == member end)
end

## Whether a copy is in the catalog and on the shelf, free to borrow.
fn available?(library: Library, copy: Text) : Bool
  library.copies.has?(copy) and not library.loans.has?(copy)
end

## The copies of a book that are on the shelf, in the order they were added.
fn available_copies(library: Library, isbn: Text) : List(Text)
  library.copies.keys.filter(fn(copy)
    library.copies.get(copy) == Some(isbn) and library.available?(copy)
  end)
end

## The book that a copy is a copy of.
fn book_of(library: Library, copy: Text) : Option(Book)
  isbn = try library.copies.get(copy)
  library.books.get(isbn)
end

## The loans that have run past the loan period at `now`, the oldest first.
fn overdue(library: Library, now: Time) : List(Loan)
  library.loans.values
    .filter(fn(loan) now - loan.since > loan_period end)
    .sort_by(fn(loan) loan.since end)
end

## For examples, promises and tests: nine in the morning on Monday 5 January
## 2026.
opening = case Time.parse("2026-01-05T09:00:00Z")
  Some(time): time
  None: stop "the opening time is valid RFC 3339"
end

## For promises and tests: a novel with two copies in the sample library.
dune = Book(isbn: "978-0441172719", title: "Dune", author: "Frank Herbert")

## For promises and tests: another novel with two copies in the sample library.
emma = Book(isbn: "978-0141439587", title: "Emma", author: "Jane Austen")

## For promises and tests: two copies of each book, two members, and nothing
## lent.
sample = Library(
  books: {dune.isbn: dune, emma.isbn: emma},
  copies: {"dune-1": dune.isbn, "dune-2": dune.isbn, "emma-1": emma.isbn, "emma-2": emma.isbn},
  members: ["ana", "ben"].to_set,
  loans: {}
)

## The sample library with these copies lent to one member at the opening
## time. It is made directly, without borrow's checks, so that promises can
## show libraries that borrow must never make.
fn lent_to(member: Text, copies: List(Text)) : Library
  var library = sample
  for copy in copies
    library.loans = library.loans.set(copy, Loan(copy: copy, member: member, since: opening))
  end
  library
end

test "a copy is out to one member until it is returned"
  assert sample.borrow("ana", "dune-1", opening) is Ok(lent)
  assert lent.loans_of("ana") == [Loan(copy: "dune-1", member: "ana", since: opening)]
  assert not lent.available?("dune-1")
  assert lent.available_copies(dune.isbn) == ["dune-2"]
  assert lent.borrow("ben", "dune-1", opening + 1.days) == Error(OnLoan(copy: "dune-1"))
  assert lent.borrow("ana", "dune-1", opening + 1.days) == Error(OnLoan(copy: "dune-1"))
  assert lent.return_copy("dune-1", opening + 10.days) is Ok(receipt)
  assert receipt.fine == 0
  assert receipt.library.available?("dune-1")
  assert receipt.library.borrow("ben", "dune-1", opening + 10.days) is Ok(_)
end

test "a member has at most three copies out at once"
  var library = sample
  for copy in ["dune-1", "dune-2", "emma-1"]
    assert library.borrow("ana", copy, opening) is Ok(more)
    library = more
  end
  assert library.loan_count("ana") == 3
  assert library.borrow("ana", "emma-2", opening) == Error(LimitReached(member: "ana"))
  assert library.borrow("ben", "emma-2", opening) is Ok(_)
  assert library.return_copy("dune-2", opening + 1.days) is Ok(receipt)
  assert receipt.library.borrow("ana", "emma-2", opening + 1.days) is Ok(_)
end

test "a copy returned late costs 0.25 for each day over 14"
  assert sample.borrow("ana", "dune-1", opening) is Ok(lent)
  assert lent.overdue(opening + 14.days) == []
  assert lent.overdue(opening + 20.days).size == 1
  assert lent.return_copy("dune-1", opening + 20.days) is Ok(receipt)
  assert receipt.fine == 1.50
  assert receipt.loan.member == "ana"
end

test "only members borrow, only copies in the catalog, and only copies out come back"
  assert sample.borrow("cy", "dune-1", opening) == Error(UnknownMember(member: "cy"))
  assert sample.borrow("ana", "kim-1", opening) == Error(UnknownCopy(copy: "kim-1"))
  assert sample.return_copy("dune-1", opening) == Error(NotLent(copy: "dune-1"))
end

test "copies are added for books in the catalog, each under its own id"
  library = empty.add_book(dune).enroll("ana")
  assert library.add_copy("emma-1", emma.isbn) == Error(UnknownBook(isbn: emma.isbn))
  assert library.add_copy("dune-1", dune.isbn) is Ok(stocked)
  assert stocked.add_copy("dune-1", dune.isbn) == Error(DuplicateCopy(copy: "dune-1"))
  assert stocked.book_of("dune-1") == Some(dune)
  assert stocked.available_copies(dune.isbn) == ["dune-1"]
  assert stocked.borrow("ana", "dune-1", opening) is Ok(_)
end
