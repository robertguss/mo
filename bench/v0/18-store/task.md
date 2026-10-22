**store** (extend a program): Your folder holds a Mo program, the module `Store` in `store.mo`, a key-value store that reads commands from standard input, one on each line, and prints one reply for each. It has three commands: `SET KEY VALUE...`, `GET KEY` and `DEL KEY`. Add three more, keeping the three as they are:

- `INCR KEY` adds 1 to the whole number held at KEY, stores the result there as text, and replies with it. A key with no value counts as 0.
- `INCRBY KEY AMOUNT` does the same, adding AMOUNT, a whole number that may be negative.
- `KEYS` replies with every key in the store, separated by single spaces, in the order they were first set: setting a key again keeps its place, and a key deleted and set again goes last. An empty store replies `(empty)`.

A whole number is an optional `-` and ASCII digits, and fits in a 64-bit signed integer. When the value at KEY, or AMOUNT, isn't one, the reply is `ERR not an integer`; when the result wouldn't fit, it is `ERR overflow`; either way the store is unchanged. As now, a command's words are separated by white space, and a line that isn't one of the six commands with the right number of words replies `ERR unknown command`. Add tests of the new commands to `store.mo`.
