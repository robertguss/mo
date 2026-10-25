# Literals (reference 3).
a = 0
b = 1_000_000
c = 0xFF_FF
d = 0b1010_0101
e = 3.14159
f = 1_000.000_1
g = 'a'
h = '\n'
i = '\''
j = '\u{1F600}'
k = 'é'
l = "plain"
m = "escapes: \n \t \r \\ \" \' \# \u{0} \u{10FFFF}"
n = "#{a} and #{b + 1} and #{f(x).g} and #{"nested #{"deeper #{z}"}"}"
o = "braces in interpolation: #{ {"k": 1}.size } and \#{not interpolated}"
p = "a # inside text is not a comment"
aa = "#{case n
  1: "one"
  _: "other"
end} item#{
  suffix
}"
q = """
  line one
    indented
  "quotes" and #{interpolation}

  after a blank line
  """
r = """
"""
s = (1, "one", 'x')
t = s.0 + pair.1.0
u = 1.5.round(2)
v = 0..10
w = -1
x = - - 1
y = true and not false
z = Unit
second = 1.second
millisecond = 1.millisecond
minute = 1.minute
hour = 1.hour
day = 1.day
plural = [2.seconds, 2.milliseconds, 2.minutes, 2.hours, 2.days]
qualified = Int.minute
