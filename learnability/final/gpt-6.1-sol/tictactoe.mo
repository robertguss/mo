module Tictactoe

winning_lines = [(0, 1, 2), (3, 4, 5), (6, 7, 8), (0, 3, 6), (1, 4, 7), (2, 5, 8), (0, 4, 8), (2, 4, 6)]

fn wins(board: List(Char), mark: Char) : Bool
  for (a, b, c) in winning_lines
    if board.get(a) == Some(mark) and board.get(b) == Some(mark) and board.get(c) == Some(mark)
      return true
    end
  end
  false
end

fn has_legal_winning_move(board: List(Char), mark: Char) : Bool
  for (index, cell) in board.enumerate
    continue if cell != mark
    if board.set(index, '.') is Some(previous)
      return true if not wins(previous, 'X') and not wins(previous, 'O')
    end
  end
  false
end

fn classify(lines: List(Text)) : Text
  return "invalid" if lines.size != 3
  var board = []
  for line in lines
    return "invalid" if line.char_count != 3
    for cell in line.chars
      return "invalid" if cell != 'X' and cell != 'O' and cell != '.'
      board = board.append(cell)
    end
  end

  x_count = board.count(fn(cell) cell == 'X' end)
  o_count = board.count(fn(cell) cell == 'O' end)
  return "invalid" if x_count != o_count and x_count != o_count + 1

  x_won = wins(board, 'X')
  o_won = wins(board, 'O')
  return "invalid" if x_won and o_won
  if x_won
    return "invalid" if x_count != o_count + 1 or not has_legal_winning_move(board, 'X')
    return "X wins"
  end
  if o_won
    return "invalid" if x_count != o_count or not has_legal_winning_move(board, 'O')
    return "O wins"
  end
  return "draw" if x_count + o_count == 9
  if x_count == o_count: "X to move" else: "O to move"
end

fn main(console: Console)
  var lines = []
  for _ in 0..3
    case console.read_line
      Some(line): lines = lines.append(line)
      None:
        console.print("invalid")
        return
    end
  end
  if console.read_line is Some(_)
    console.print("invalid")
    return
  end
  console.print(classify(lines))
end
