## Tests of the Grades module.
module GradesTest
use Grades{Student}

test "a line names a student and lists scores"
  assert Grades.parse_line("Ada: 90, 85, 77") == Some(Student(name: "Ada", scores: [90, 85, 77]))
  assert Grades.parse_line("  Grace Hopper :100") ==
    Some(Student(name: "Grace Hopper", scores: [100]))
  assert Grades.parse_line("Bo:") == Some(Student(name: "Bo", scores: []))
end

test "a line that isn't a name and scores from 0 to 100 can't be read"
  assert Grades.parse_line("Ada 90") == None
  assert Grades.parse_line(": 90") == None
  assert Grades.parse_line("Ada: 90, 101") == None
  assert Grades.parse_line("Ada: 90, -1") == None
  assert Grades.parse_line("Ada: 90,, 80") == None
  assert Grades.parse_line("Ada: 9.5") == None
  assert Grades.parse_line("Ada: 1: 2") == None
end

test "averages are rounded to one place"
  assert Grades.average([90, 85]) == Some(87.5)
  assert Grades.average([1, 1, 2]) == Some(1.3)
  assert Grades.average([100]) == Some(100)
  assert Grades.average([]) == None
end

test "letters follow the usual bands"
  assert Grades.letter(100) == "A"
  assert Grades.letter(90) == "A"
  assert Grades.letter(89.9) == "B"
  assert Grades.letter(80) == "B"
  assert Grades.letter(70) == "C"
  assert Grades.letter(60) == "D"
  assert Grades.letter(59.9) == "F"
  assert Grades.letter(0) == "F"
end

test "a report gives the average and the letter"
  assert Grades.report(Student(name: "Ada", scores: [90, 85, 77])) == "Ada: 84 B"
  assert Grades.report(Student(name: "Bo", scores: [])) == "Bo: no scores"
end

test "main reports each student and each line it can't read"
  console = Console.fake_input(["Ada: 90, 85", "", "Bo:", "Cy: 9x", "Di: 59, 60"])
  assert Grades.main(console) == 1
  assert console.printed == ["Ada: 87.5 B", "Bo: no scores", "Di: 59.5 F"]
  assert console.printed_errors == ["line 4: can't read Cy: 9x"]
end
