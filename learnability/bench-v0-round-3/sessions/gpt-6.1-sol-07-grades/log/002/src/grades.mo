## Reads students' scores from standard input and prints each one's average and grade.
module Grades
expose Student(..), parse_line, average, letter, report, main

struct Student
  name: Text
  scores: List(Int)
end

## Reads a line such as `Ada: 90, 85, 77`; every score is from 0 to 100.
fn parse_line(line: Text) : Option(Student)
  examples
    parse_line("Ada: 90, 85") == Some(Student(name: "Ada", scores: [90, 85]))
    parse_line("Ada: 101") == None
    parse_line("Ada 90") == None
    parse_line("Bo:") == Some(Student(name: "Bo", scores: []))
  end

  parts = line.split(":")
  return None if parts.size != 2
  name = (parts.get(0) or "").trim
  return None if name.empty?
  rest = (parts.get(1) or "").trim
  return Some(Student(name: name, scores: [])) if rest.empty?
  var scores = []
  for field in rest.split(",")
    score = field.trim.to_int
    return None if score < 0 || score > 100
    scores = scores.append(score)
  end
  Some(Student(name: name, scores: scores))
end

## The average of the scores, rounded to one decimal place, or `None` with no scores.
fn average(scores: List(Int)) : Option(Decimal)
  examples
    average([90, 85]) == Some(87.5)
    average([1, 1, 2]) == Some(1.3)
    average([]) == None
  end

  return None if len(scores) == 0
  count = scores.size
  total = scores.reduce(0, fn(sum, score) sum + score end)
  Some((total.to_decimal / scores.size.to_decimal).round(1))
end

fn letter(average: Decimal) : Text
  examples
    letter(90) == "A"
    letter(89.9) == "B"
    letter(59.9) == "F"
  end

  if average >= 90
    "A"
  else if average >= 80
    "B"
  else if average >= 70
    "C"
  else if average >= 60
    "D"
  else
    "F"
  end
end

fn report(student: Student) : Text
  examples
    report(Student(name: "Ada", scores: [90, 85])) == "Ada: 87.5 B"
    report(Student(name: "Bo", scores: [])) == "Bo: no scores"
  end

  case average(student.scores)
    Some(mean): "#{student.name}: #{mean} #{letter(mean)}"
    None: student.name + ": no scores"
  end
end

fn main(console: Console) : Int
  var status = 0
  number = 0
  while console.read_line is Some(line)
    number += 1
    continue if line.trim.empty?
    case parse_line(line)
      Some(student): console.print(report(student))
      None:
        console.error("line #{number}: can't read #{line}")
        status = 1
    end
  end
  status
end
