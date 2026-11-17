import itertools, random, sys
SEP = "\x1d"
hand = r'''
""
"a"
"a\"b"
"a\\"
"a\\\"b"
"\\\\"
"\\\\\""
"é😀"
"\ud83d"
"\ud83dx"
"\udc00"
"\x"
"\u12"
"\u12G4"
"tab	inside"
"café ☕ 😀"
["a","b\"c","d"]
{"a":"b","c":"d\\","e":"f"}
{"a\"":1,"a\"":2}
{"a":1,"a":2}
{"":""}
{"a":{"b":{"c":[1,2,{"d":"e"}]}}}
[1,2
[1,2,]
[,]
{"a"}
{"a":}
{"a":1,}
{1:2}
"abc
"abc\
"abc\"
"
""x
"a""b"
[1"a"]
["a" "b"]
["a",,"b"]
nul
null
nullx
true
tru
false
falsy
0
-0
01
1.
1.5e
1.5e+3
-
1e5x
 [ 1 , "a" , { "k" : null } ] 
[1]
 x
[[[[[[[[[[1]]]]]]]]]]
{"k":"line
break"}
{"k":"cr
break"}
'''.strip("\n").split("\n")
docs = list(hand)
docs.append("[" * 129)
docs.append("[" * 128 + "]" * 128)
docs.append('{"a":' * 129)
docs.append("")
docs.append("   ")
docs.append('"a\x00b"')
docs.append('"a\x1fb"')
docs.append('["x\x7fy"]')
docs.append('[\n  "a",\n  {"b": "c\\n"},\n\t"d"\r\n]')
seed = '{"id":7,"name":"Zoë \\"Z\\" Q","tags":["a","","é☕"],"ok":true,"n":null,"x":-1.5e+3,"p":"a\\\\","u":"\\u00e9\\ud83d\\ude00","o":{"k":[1,{"m":"v"}]}}'
docs.append(seed)
for cut in range(len(seed)):
    docs.append(seed[:cut])
alphabet = ['"', "\\", "\n", "\t", "{", "}", "[", "]", ",", ":", "0", "a", "\x01", " ", "u", "-", "é"]
for at in range(len(seed)):
    for c in alphabet:
        docs.append(seed[:at] + c + seed[at + 1:])
        docs.append(seed[:at] + c + seed[at:])
    docs.append(seed[:at] + seed[at + 1:])
random.seed(20261117)
for _ in range(3000):
    text = list(seed)
    for _ in range(random.randint(2, 4)):
        at = random.randrange(len(text) + 1)
        kind = random.random()
        if kind < 0.4 and at < len(text):
            text[at] = random.choice(alphabet)
        elif kind < 0.8:
            text.insert(at, random.choice(alphabet))
        elif at < len(text):
            del text[at]
    docs.append("".join(text))
for length in range(8):
    for marks in itertools.product('"\\a[],', repeat=length):
        docs.append("".join(marks))
pieces = ['"', "\\", "a", "[", "]", ",", "\\u0022", ":", "{", "}", "\n", "\x00", "\x1f", "\x7f", "e\u0301", " "]
for length in range(1, 5):
    for parts in itertools.product(pieces, repeat=length):
        docs.append("".join(parts))
frames = ['[%s,"z"]', '{"k":%s,"z":"y"}', '{%s:1,"z":"y"}', '["p",%s,"q\\\\",%s,"r"]']
inside = ['"', "\\", '\\"', "a", "\\u0022", "\\\\", "\x00", "\x1f", "\x7f", "\t", "e\u0301", "\U0001F468\u200d\U0001F469", "\\ud83d\\ude00", " "]
for frame in frames:
    for length in range(4):
        for parts in itertools.product(inside, repeat=length):
            docs.append(frame.replace("%s", '"' + "".join(parts) + '"'))
assert all(SEP not in d for d in docs)
sys.stdout.buffer.write(SEP.join(docs).encode())
sys.stderr.write(f"{len(docs)} documents\n")
