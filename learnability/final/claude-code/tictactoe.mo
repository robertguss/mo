## Tic-tac-toe: reads a 3 by 3 board from standard input, as three lines of
## `X`, `O` and `.`, with X moving first, and prints what the board says about
## its game: `X wins`, `O wins`, `draw`, `X to move`, `O to move`, or `invalid`
## for a board that no game can reach.
module Tictactoe

## What a board says about its game.
enum Outcome
  Wins(Char)
  Draw
  ToMove(Char)
  Invalid
end

## The eight lines of three cells that win a game. Cells are numbered 0 to 8,
## row by row from the top left.
winning_lines = [
  [0, 1, 2], [3, 4, 5], [6, 7, 8],
  [0, 3, 6], [1, 4, 7], [2, 5, 8],
  [0, 4, 8], [2, 4, 6]
]

## Prints what the board on standard input says about its game.
fn main(console: Console)
  var lines = []
  while console.read_line is Some(line)
    lines = lines.append(line)
  end
  outcome = judge(lines)
  console.print("#{outcome}")
end

## What the board written in these lines of input says about its game.
fn judge(lines: List(Text)) : Outcome
  examples
    judge(["XXX", "OO.", "..."]) == Wins('X')
    judge(["XX.", "OOO", "X.."]) == Wins('O')
    judge(["XOX", "XOO", "OXX"]) == Draw
    judge(["...", "...", "..."]) == ToMove('X')
    judge(["X..", ".O.", "..X"]) == ToMove('O')
    judge(["XOX", "XOO", "OXX", ""]) == Draw
    judge(["XO", "...", "..."]) == Invalid
    judge(["XOX", "OXO"]) == Invalid
    judge(["XOX", "OXO", "XOX", "..."]) == Invalid
    judge(["xo.", "...", "..."]) == Invalid
    judge(["OO.", "X..", "..."]) == Invalid
    judge(["XXX", "OO.", "O.."]) == Invalid
  end

  case read_board(lines)
    Some(cells): assess(cells)
    None: Invalid
  end
end

## The nine cells of the board, row by row, or `None` when the lines aren't
## three rows of three cells, each `X`, `O` or `.`. Blank lines after the board
## are ignored.
fn read_board(lines: List(Text)) : Option(List(Char))
  rows = without_trailing_blanks(lines)
  return None if rows.size != 3 or not rows.all?(valid_row?)
  Some(rows.flat_map(fn(row) row.chars end))
end

## The lines, without any blank lines at their end.
fn without_trailing_blanks(lines: List(Text)) : List(Text)
  promise "only blank lines are dropped, and every one of them at the end"
    lines.take(result.size) == result
    lines.drop(result.size).all?(fn(dropped) dropped.trim.empty? end)
    result.last is Some(kept) implies not kept.trim.empty?
    allows lines: ["XOX", " "], result: ["XOX"]
    forbids lines: ["XOX", " "], result: []
  end

  case lines.last
    Some(line) if line.trim.empty?:
      without_trailing_blanks(lines.take(lines.size - 1))
    Some(_) | None: lines
  end
end

## Whether the row is exactly three cells, each `X`, `O` or `.`.
fn valid_row?(row: Text) : Bool
  cells = row.chars
  cells.size == 3 and
    cells.all?(fn(cell) cell == 'X' or cell == 'O' or cell == '.' end)
end

## What a well-formed board of nine cells says about its game.
fn assess(cells: List(Char)) : Outcome
  crosses = cells.count(fn(cell) cell == 'X' end)
  noughts = cells.count(fn(cell) cell == 'O' end)
  x_won? = has_line?(cells, 'X')
  o_won? = has_line?(cells, 'O')
  return Invalid if crosses != noughts and crosses != noughts + 1
  return Invalid if x_won? and o_won?
  return Invalid if x_won? and crosses == noughts    # O moved after X had won
  return Invalid if o_won? and crosses > noughts     # X moved after O had won
  return Wins('X') if x_won?
  return Wins('O') if o_won?
  return Draw if crosses + noughts == 9
  if crosses == noughts: ToMove('X') else: ToMove('O')
end

## Whether the player holds all three cells of a winning line.
fn has_line?(cells: List(Char), player: Char) : Bool
  examples
    has_line?("X..X..X..".chars, 'X')
    has_line?("..O.O.O..".chars, 'O')
    not has_line?("XX.X...O.".chars, 'X')
  end

  winning_lines.any?(fn(line)
    line.all?(fn(position) cells.get(position) == Some(player) end)
  end)
end

## The text printed for an outcome.
fn to_text(outcome: Outcome) : Text
  examples
    to_text(Wins('O')) == "O wins"
    to_text(ToMove('X')) == "X to move"
    to_text(Draw) == "draw"
    to_text(Invalid) == "invalid"
  end

  case outcome
    Wins(player): "#{player} wins"
    Draw: "draw"
    ToMove(player): "#{player} to move"
    Invalid: "invalid"
  end
end

test "main reads the board from standard input and prints whose move it is"
  console = Console.fake_input(["XOX", "OXO", "..."])
  main(console)
  assert console.printed == ["X to move"]
end

test "a board where X moved after O had won is invalid"
  console = Console.fake_input(["OOO", "XX.", "XX."])
  main(console)
  assert console.printed == ["invalid"]
end

test "a full board with a line of three is a win, not a draw"
  assert judge(["XOX", "OXO", "OXX"]) == Wins('X')
end

test "missing or wrong characters make the board invalid"
  assert judge([]) == Invalid
  assert judge(["XO.", "...", ".-."]) == Invalid
  assert judge(["XO.", "... ", "..."]) == Invalid
end
