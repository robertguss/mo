## Lending-library core. Callers keep the latest returned Library value.
module Library
expose Library, BookCopy(..), Loan(..), LibraryError(..), new, add_copy,
  register_member, borrow, return_copy, copies, loans_for, loan_for, fine

struct BookCopy
  id: Text
  title: Text
end

struct Loan
  member_id: Text
  borrowed_at: Time
end

struct Library
  catalog: Map(Text, BookCopy)
  members: Set(Text)
  loans: Map(Text, Loan)
end

enum LibraryError
  DuplicateCopy(copy_id: Text)
  DuplicateMember(member_id: Text)
  UnknownCopy(copy_id: Text)
  UnknownMember(member_id: Text)
  CopyOnLoan(copy_id: Text)
  LoanLimitReached(member_id: Text)
  CopyNotOnLoan(copy_id: Text)
  WrongBorrower(copy_id: Text)
  ReturnBeforeBorrow
end

fn new() : Library
  Library(catalog: {}, members: Set.empty, loans: {})
end

fn add_copy(library: Library, copy: BookCopy) : Result(Library, LibraryError)
  return Error(DuplicateCopy(copy_id: copy.id)) if library.catalog.has?(copy.id)
  var updated = library
  updated.catalog = updated.catalog.set(copy.id, copy)
  Ok(updated)
end

fn register_member(library: Library, member_id: Text) : Result(Library, LibraryError)
  return Error(DuplicateMember(member_id: member_id)) if library.members.has?(member_id)
  var updated = library
  updated.members = updated.members.add(member_id)
  Ok(updated)
end

fn copies(library: Library) : List(BookCopy)
  library.catalog.values
end

## Returns (copy ID, loan) pairs for this member.
fn loans_for(library: Library, member_id: Text) : List((Text, Loan))
  library.loans.entries.filter(fn(entry) entry.1.member_id == member_id end)
end

fn loan_for(library: Library, copy_id: Text) : Option(Loan)
  library.loans.get(copy_id)
end

fn borrow(library: Library, member_id: Text, copy_id: Text, now: Time) : Result(Library, LibraryError)
  promise "a copy is never lent to two members at once; existing loans are never replaced"
    result is Ok(single) implies (
      not library.loans.has?(copy_id) and
      single.loans == library.loans.set(copy_id, Loan(member_id: member_id, borrowed_at: now))
    )
    allows library: anchor_two, member_id: "ana", copy_id: "c3",
      now: anchor_time, result: Ok(anchor_three)
    forbids library: anchor_three, member_id: "ben", copy_id: "c1",
      now: anchor_time, result: Ok(Library(
        catalog: anchor_three.catalog,
        members: anchor_three.members,
        loans: anchor_three.loans.set("c1", Loan(member_id: "ben", borrowed_at: anchor_time))
      ))
  end
  promise "a member never has more than three loans"
    result is Ok(limited) implies limited.members.to_list.all?(fn(id)
      loans_for(limited, id).size <= 3
    end)
    allows library: anchor_two, member_id: "ana", copy_id: "c3",
      now: anchor_time, result: Ok(anchor_three)
    forbids library: anchor_three, member_id: "ana", copy_id: "c4",
      now: anchor_time, result: Ok(Library(
        catalog: anchor_three.catalog,
        members: anchor_three.members,
        loans: anchor_three.loans.set("c4", Loan(member_id: "ana", borrowed_at: anchor_time))
      ))
  end

  return Error(UnknownMember(member_id: member_id)) if not library.members.has?(member_id)
  return Error(UnknownCopy(copy_id: copy_id)) if not library.catalog.has?(copy_id)
  return Error(CopyOnLoan(copy_id: copy_id)) if library.loans.has?(copy_id)
  return Error(LoanLimitReached(member_id: member_id)) if loans_for(library, member_id).size >= 3
  var updated = library
  updated.loans = updated.loans.set(copy_id, Loan(member_id: member_id, borrowed_at: now))
  Ok(updated)
end

## Returns the new state and the fine charged for this return.
fn return_copy(library: Library, member_id: Text, copy_id: Text, now: Time) : Result((Library, Decimal), LibraryError)
  return Error(UnknownMember(member_id: member_id)) if not library.members.has?(member_id)
  return Error(UnknownCopy(copy_id: copy_id)) if not library.catalog.has?(copy_id)
  loan = try library.loans.get(copy_id).or_fail(CopyNotOnLoan(copy_id: copy_id))
  return Error(WrongBorrower(copy_id: copy_id)) if loan.member_id != member_id
  return Error(ReturnBeforeBorrow) if now < loan.borrowed_at
  charge = fine(now - loan.borrowed_at)
  var updated = library
  updated.loans = updated.loans.remove(copy_id)
  Ok((updated, charge))
end

## Fourteen days are free; each started overdue day costs exactly 0.25.
fn fine(elapsed: Duration) : Decimal
  examples
    fine(0.days) == 0
    fine(13.days) == 0
    fine(14.days) == 0
    fine(14.days + 1.seconds) == 0.25
    fine(15.days) == 0.25
    fine(15.days + 1.seconds) == 0.50
    fine(18.days) == 1.00
  end

  return 0 if elapsed <= 14.days
  overdue = elapsed - 14.days
  (overdue.in_seconds / 86400).ceil * 0.25
end

# Concrete values for the promises' allows and forbids anchors.
anchor_time = case Time.parse("2026-01-01T00:00:00Z")
  Some(parsed): parsed
  None: stop "the anchor timestamp is a valid RFC 3339 literal"
end

anchor_base = Library(
  catalog: {
    "c1": BookCopy(id: "c1", title: "First book"),
    "c2": BookCopy(id: "c2", title: "Second book"),
    "c3": BookCopy(id: "c3", title: "Third book"),
    "c4": BookCopy(id: "c4", title: "Fourth book")
  },
  members: ["ana", "ben"].to_set,
  loans: {}
)

anchor_two = Library(
  catalog: anchor_base.catalog,
  members: anchor_base.members,
  loans: {
    "c1": Loan(member_id: "ana", borrowed_at: anchor_time),
    "c2": Loan(member_id: "ana", borrowed_at: anchor_time)
  }
)

anchor_three = Library(
  catalog: anchor_base.catalog,
  members: anchor_base.members,
  loans: anchor_two.loans.set("c3", Loan(member_id: "ana", borrowed_at: anchor_time))
)

test "borrowing refuses a second borrower and a fourth loan"
  assert borrow(anchor_two, "ana", "c3", anchor_time) is Ok(third)
  assert loans_for(third, "ana").size == 3
  assert borrow(third, "ben", "c1", anchor_time) == Error(CopyOnLoan(copy_id: "c1"))
  assert borrow(third, "ana", "c4", anchor_time) == Error(LoanLimitReached(member_id: "ana"))
  assert loan_for(third, "c1") == Some(Loan(member_id: "ana", borrowed_at: anchor_time))
end

test "only the borrower may return a copy, and a return frees a loan slot"
  assert return_copy(anchor_three, "ben", "c1", anchor_time) == Error(WrongBorrower(copy_id: "c1"))
  assert return_copy(anchor_three, "ana", "c1", anchor_time - 1.seconds) == Error(ReturnBeforeBorrow)
  assert return_copy(anchor_three, "ana", "c1", anchor_time + 17.days) is Ok(receipt)
  assert receipt.1 == 0.75
  assert loan_for(receipt.0, "c1") == None
  assert loans_for(receipt.0, "ana").size == 2
  assert return_copy(receipt.0, "ana", "c1", anchor_time + 17.days) == Error(CopyNotOnLoan(copy_id: "c1"))
  assert borrow(receipt.0, "ana", "c4", anchor_time + 17.days) is Ok(replaced)
  assert loans_for(replaced, "ana").size == 3
  assert borrow(replaced, "ben", "c1", anchor_time + 17.days) is Ok(relent)
  assert loan_for(relent, "c1") == Some(Loan(member_id: "ben", borrowed_at: anchor_time + 17.days))
end

test "catalog and membership are built through the public operations"
  assert add_copy(new(), BookCopy(id: "one", title: "A book")) is Ok(stocked)
  assert add_copy(stocked, BookCopy(id: "one", title: "Another book")) == Error(DuplicateCopy(copy_id: "one"))
  assert register_member(stocked, "reader") is Ok(registered)
  assert register_member(registered, "reader") == Error(DuplicateMember(member_id: "reader"))
  assert copies(registered) == [BookCopy(id: "one", title: "A book")]
  assert borrow(registered, "stranger", "one", anchor_time) == Error(UnknownMember(member_id: "stranger"))
  assert borrow(registered, "reader", "missing", anchor_time) == Error(UnknownCopy(copy_id: "missing"))
  assert borrow(registered, "reader", "one", anchor_time) is Ok(borrowed)
  assert return_copy(borrowed, "reader", "one", anchor_time + 14.days) is Ok(receipt)
  assert receipt.1 == 0
  assert loans_for(receipt.0, "reader").empty?
end
