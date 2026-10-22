**todo** (extend a program): Your folder holds a Mo program, the module `Todo` in `todo.mo`, a to-do list kept in the file `todo.txt` in the current folder. It has three commands: `todo add TEXT...`, `todo list` and `todo done N`. Add two more, keeping the three as they are:

- `todo edit N TEXT...` replaces the text of item N, keeping whether it is done, and prints `edited N: TEXT`. The words of TEXT are joined with single spaces, as `add` joins them, and at least one is needed.
- `todo clear` removes every done item, and prints `cleared K done items`, or `cleared 1 done item` when K is 1. The items left keep their order, and are numbered again from 1.

Like `done`, `edit` reports an N that isn't the number of an item on standard error as `no item N`, changes nothing, and exits with status 1. A command line that is none of the five commands, such as `todo edit 2` or `todo clear all`, prints the usage line on standard error and exits with status 1, as now; the usage line becomes `usage: todo add TEXT... | todo list | todo done N | todo edit N TEXT... | todo clear`. Add tests of the two new commands to `todo.mo`.
